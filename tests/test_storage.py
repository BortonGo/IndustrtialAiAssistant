"""Integration tests use a new private PostgreSQL cluster, never an existing database."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'storage_tools'))
from runtime import LocalPostgres
from repository import Repository


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = ROOT / 'build-storage-check'
        directory.mkdir(exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(dir=directory)
        cls.work = Path(cls.tmp.name)
        cls.model = cls.work / 'model-fixture.gguf'
        cls.model.write_bytes(b'model identity for storage tests')
        cls.runtime = LocalPostgres(ROOT, cls.work / 'data', 55434)
        cls.options = cls.runtime.start()
        cls.repo = Repository(cls.options, cls.work / 'data', cls.model)

    @classmethod
    def tearDownClass(cls):
        cls.repo.close()
        cls.runtime.close()
        cls.tmp.cleanup()

    def chat(self):
        chat_id = str(uuid.uuid4())
        self.repo.create_chat({'id': chat_id, 'title': 'Тест чата'})
        return chat_id

    def setUp(self):
        # This cluster was created by setUpClass inside a fresh temporary directory.
        self.repo.connection.execute('TRUNCATE chats CASCADE')
        self.repo.connection.execute('DELETE FROM app_settings')

    def document(self):
        doc_id = str(uuid.uuid4())
        source = self.work / (doc_id + '.png')
        source.write_bytes(b'image fixture')
        text = '[Описание изображения моделью; возможны ошибки]\nPUMP-417: 7.5 bar'
        document = {'id': doc_id, 'sourcePath': str(source), 'text': text, 'warnings': [],
                    'blocks': [{'type': 'image', 'text': 'PUMP-417: 7.5 bar', 'pageNumber': 0, 'imagePath': str(source)}]}
        entry = {'chunk': {'documentId': doc_id, 'text': text, 'startOffset': 0, 'imagePath': str(source)},
                 'embedding': [1.0] + [0.0] * 767}
        return document, [entry]

    def test_chat_documents_are_isolated_and_images_survive_source_removal(self):
        a, b = self.chat(), self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': a, 'document': copy.deepcopy(document), 'entries': entries})
        Path(document['sourcePath']).unlink()
        first = self.repo.load_chat({'chatId': a})
        second = self.repo.load_chat({'chatId': b})
        self.assertEqual(len(first['entries']), 1)
        self.assertEqual(first['documents'][0]['id'], document['id'])
        self.assertTrue(Path(first['documents'][0]['blocks'][0]['imagePath']).is_file())
        self.assertEqual(second, {'documents': [], 'entries': []})

    def test_missing_saved_image_only_disables_its_document_and_can_recover(self):
        chat = self.chat()
        damaged, vectors = self.document()
        self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(damaged), 'entries': vectors})
        healthy, healthy_vectors = self.document()
        healthy['text'] = 'Independent text document'
        healthy['blocks'] = [{'type': 'text', 'text': healthy['text'], 'pageNumber': 0}]
        healthy_vectors[0]['chunk']['text'] = healthy['text']
        healthy_vectors[0]['chunk'].pop('imagePath')
        self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(healthy), 'entries': healthy_vectors})
        before = self.repo.load_chat({'chatId': chat})
        stored = next(d for d in before['documents'] if d['id'] == damaged['id'])
        image = Path(stored['blocks'][0]['imagePath'])
        content = image.read_bytes()
        image.unlink()
        restored = self.repo.load_chat({'chatId': chat})
        documents = {d['id']: d for d in restored['documents']}
        self.assertEqual(documents[damaged['id']]['status'], 'Error')
        self.assertTrue(any(str(image) in warning for warning in documents[damaged['id']]['warnings']))
        self.assertEqual(documents[healthy['id']]['status'], 'Ready')
        self.assertEqual({e['chunk']['documentId'] for e in restored['entries']}, {healthy['id']})
        self.assertEqual(len(documents), 2)
        image.write_bytes(content)
        recovered = self.repo.load_chat({'chatId': chat})
        self.assertTrue(all(d['status'] == 'Ready' and not d['warnings'] for d in recovered['documents']))
        self.assertEqual(len(recovered['entries']), 2)

    def test_index_replacement_is_atomic_and_dimension_checked(self):
        chat_id = self.chat()
        document, entries = self.document()
        payload = {'chatId': chat_id, 'document': document, 'entries': entries}
        self.repo.save_document(copy.deepcopy(payload))
        bad = copy.deepcopy(payload)
        bad['entries'][0]['embedding'] = [1.0, 2.0]
        with self.assertRaisesRegex(ValueError, 'размерность'):
            self.repo.save_document(bad)
        bad = copy.deepcopy(payload)
        bad['document']['blocks'][0]['type'] = 'invalid'
        with self.assertRaises(Exception):
            self.repo.save_document(bad)
        restored = self.repo.load_chat({'chatId': chat_id})
        self.assertEqual(len(restored['entries']), 1)
        self.assertEqual(restored['documents'][0]['blocks'][0]['type'], 'image')
        self.assertEqual(len(restored['entries'][0]['embedding']), 768)

    def test_completed_answer_context_and_idempotency(self):
        chat_id, question_id, answer_id = self.chat(), str(uuid.uuid4()), str(uuid.uuid4())
        self.repo.save_question({'id': question_id, 'chatId': chat_id, 'text': 'Давление?', 'title': 'Давление?'})
        self.repo.save_context({'id': question_id, 'context': 'Источник: табличка\n7.5 bar'})
        answer = {'questionId': question_id, 'id': answer_id, 'text': '7.5 bar'}
        self.repo.finish_question(answer)
        self.repo.finish_question(answer)
        data = self.repo.bootstrap({})
        messages = next(c for c in data['chats'] if str(c['id']) == chat_id)['messages']
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]['status'], 'complete')
        self.assertIn('табличка', messages[0]['context_snapshot'])
        self.assertEqual(str(messages[1]['reply_to']), question_id)

    def test_different_embedding_profile_is_not_used(self):
        chat_id = self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': chat_id, 'document': document, 'entries': entries})
        original = self.repo.profile
        try:
            self.repo.profile = 'different-model'
            result = self.repo.load_chat({'chatId': chat_id})
            self.assertEqual(result['entries'], [])
            self.assertEqual(result['documents'][0]['status'], 'Error')
        finally:
            self.repo.profile = original

    def test_partial_answer_is_saved_as_failed_and_excluded_from_history(self):
        chat_id = self.chat()
        question, answer_id = str(uuid.uuid4()), str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Разбери документ', 'title': 'Проверка'})
        payload = {'questionId': question, 'id': answer_id, 'text': 'Первый пункт.\n', 'error': 'Соединение прервано'}
        self.repo.finish_question(payload)
        self.repo.finish_question(payload)
        messages = self.repo.bootstrap({})['chats'][0]['messages']
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[1]['text'], payload['text'])
        self.assertEqual(messages[1]['status'], 'failed')
        self.assertEqual(messages[1]['error'], payload['error'])
        next_question = str(uuid.uuid4())
        self.repo.save_question({'id': next_question, 'chatId': chat_id, 'text': 'Следующий вопрос', 'title': 'Проверка'})
        self.assertEqual(self.repo.question_history({'questionId': next_question, 'chatId': chat_id}), {'pairs': []})

    def test_message_attachments_survive_restart_source_removal_and_reindex(self):
        chat_id = self.chat()
        document, entries = self.document()
        source = document['sourcePath']
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Что здесь?', 'title': 'Фото', 'attachments': [source]})
        self.repo.save_document({'chatId': chat_id, 'document': copy.deepcopy(document), 'entries': entries})
        bound = self.repo.bind_attachment({'questionId': question, 'position': 0, 'documentId': document['id']})['attachments'][0]
        self.assertTrue(Path(bound['previewPath']).is_file())
        self.repo.save_document({'chatId': chat_id, 'document': copy.deepcopy(document), 'entries': entries})
        self.repo.finish_question({'questionId': question, 'id': str(uuid.uuid4()), 'text': 'PUMP-417'})
        Path(source).unlink()
        self.repo.close()
        self.__class__.repo = Repository(self.options, self.work / 'data', self.model)
        restored = self.repo.bootstrap({})['chats'][0]['messages'][0]['attachments']
        self.assertEqual(restored, [bound])
        next_question = str(uuid.uuid4())
        self.repo.save_question({'id': next_question, 'chatId': chat_id, 'text': 'А давление?', 'title': 'Фото'})
        history = self.repo.question_history({'questionId': next_question, 'chatId': chat_id})
        self.assertIn(Path(source).name, history['pairs'][0]['question'])
        self.repo.finish_question({'questionId': next_question, 'error': 'Test finished'})
        self.repo.delete_chat({'chatId': chat_id})
        self.assertFalse(Path(bound['previewPath']).exists())

    def test_attachment_cannot_bind_another_chat_or_another_file(self):
        a, b = self.chat(), self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': a, 'document': copy.deepcopy(document), 'entries': entries})
        for chat_id, source in [(b, document['sourcePath']), (a, str(self.work / 'different.png'))]:
            question = str(uuid.uuid4())
            self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Что здесь?', 'title': 'Фото', 'attachments': [source]})
            with self.assertRaises(ValueError):
                self.repo.bind_attachment({'questionId': question, 'position': 0, 'documentId': document['id']})
            self.repo.finish_question({'questionId': question, 'error': 'Expected attachment failure'})
        with self.assertRaises(ValueError):
            self.repo.save_question({'id': str(uuid.uuid4()), 'chatId': a, 'text': 'Too many', 'title': 'Test', 'attachments': ['x'] * 5})

    def test_failed_attachment_metadata_remains_visible(self):
        chat_id = self.chat()
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Read file', 'title': 'Test', 'attachments': ['broken.pdf']})
        self.repo.finish_question({'questionId': question, 'error': 'Unreadable file'})
        message = self.repo.bootstrap({})['chats'][0]['messages'][0]
        self.assertEqual(message['status'], 'failed')
        self.assertEqual(message['attachments'][0]['fileName'], 'broken.pdf')
        self.assertEqual(message['attachments'][0]['documentId'], '')

    def test_attachment_migration_preserves_old_messages(self):
        chat_id = self.chat()
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'До вложений', 'title': 'Старый чат'})
        self.repo.finish_question({'questionId': question, 'id': str(uuid.uuid4()), 'text': 'Старый ответ'})
        with self.repo.connection.transaction():
            self.repo.connection.execute('DROP TABLE message_attachments')
            self.repo.connection.execute('DELETE FROM schema_migrations WHERE version=3')
        self.repo.migrate()
        self.repo.migrate()
        messages = self.repo.bootstrap({})['chats'][0]['messages']
        self.assertEqual([m['text'] for m in messages], ['До вложений', 'Старый ответ'])
        self.assertTrue(all(m['attachments'] == [] for m in messages))

    def test_deletion_keeps_another_messages_old_photo_after_reindex(self):
        a, b = self.chat(), self.chat()
        previews = []
        for chat_id in (a, b):
            document, entries = self.document()
            self.repo.save_document({'chatId': chat_id, 'document': copy.deepcopy(document), 'entries': entries})
            question = str(uuid.uuid4())
            self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Фото', 'title': 'Фото', 'attachments': [document['sourcePath']]})
            previews.append(self.repo.bind_attachment({'questionId': question, 'position': 0, 'documentId': document['id']})['attachments'][0]['previewPath'])
            self.repo.finish_question({'questionId': question, 'id': str(uuid.uuid4()), 'text': 'Описание фото'})
        self.assertEqual(previews[0], previews[1])
        Path(document['sourcePath']).write_bytes(b'changed photo content')
        self.repo.save_document({'chatId': b, 'document': copy.deepcopy(document), 'entries': entries})
        self.repo.delete_chat({'chatId': a})
        self.assertTrue(Path(previews[1]).is_file())
        self.repo.delete_chat({'chatId': b})
        self.assertFalse(Path(previews[1]).exists())

    def test_interrupted_answer_migration_preserves_existing_history(self):
        chat_id = self.chat()
        question, answer_id = str(uuid.uuid4()), str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Старый вопрос', 'title': 'История'})
        self.repo.finish_question({'questionId': question, 'id': answer_id, 'text': 'Старый ответ'})
        # Reconstruct version 1 only inside this test's private cluster.
        with self.repo.connection.transaction():
            self.repo.connection.execute('ALTER TABLE messages DROP CONSTRAINT messages_role_reply_status')
            self.repo.connection.execute("ALTER TABLE messages ADD CONSTRAINT messages_check CHECK ((role='user' AND reply_to IS NULL) OR (role='assistant' AND reply_to IS NOT NULL AND status='complete'))")
            self.repo.connection.execute('DELETE FROM schema_migrations WHERE version=2')
        self.repo.migrate()
        self.repo.migrate()
        messages = self.repo.bootstrap({})['chats'][0]['messages']
        self.assertEqual([message['text'] for message in messages], ['Старый вопрос', 'Старый ответ'])
        self.assertTrue(all(message['status'] == 'complete' for message in messages))

    def test_restart_restores_messages_and_marks_interrupted_question(self):
        chat_id, question_id = self.chat(), str(uuid.uuid4())
        self.repo.save_question({'id': question_id, 'chatId': chat_id, 'text': 'Прерванный вопрос', 'title': 'Прерванный вопрос'})
        self.repo.close()
        self.runtime.close()
        self.options = self.runtime.start()
        type(self).repo = Repository(self.options, self.work / 'data', self.model)
        data = self.repo.bootstrap({})
        chat = next(c for c in data['chats'] if str(c['id']) == chat_id)
        self.assertEqual(chat['messages'][0]['status'], 'failed')
        self.assertIn('прерван', chat['messages'][0]['error'])

    def test_second_instance_cannot_take_or_stop_the_active_cluster(self):
        other = LocalPostgres(ROOT, self.work / 'data', 55434)
        try:
            with self.assertRaisesRegex(RuntimeError, 'другим экземпляром'):
                other.start()
        finally:
            other.close()
        self.assertTrue(self.repo.bootstrap({})['chats'])

    def test_history_contains_only_complete_prior_pairs_from_the_requested_chat(self):
        a, b = self.chat(), self.chat()
        def question(chat, text):
            identity = str(uuid.uuid4())
            self.repo.save_question({'id': identity, 'chatId': chat, 'text': text, 'title': text})
            return identity
        q = question(a, 'Код проекта: AZ-781')
        self.repo.finish_question({'questionId': q, 'id': str(uuid.uuid4()), 'text': 'Код запомнен'})
        other = question(b, 'Чужой секрет B')
        self.repo.finish_question({'questionId': other, 'id': str(uuid.uuid4()), 'text': 'Ответ B'})
        failed = question(a, 'Неудачный вопрос')
        self.repo.finish_question({'questionId': failed, 'error': 'Отмена'})
        question(a, 'Ещё нет ответа')
        current = question(a, 'Какой код?')
        future = question(a, 'Будущий вопрос')
        self.repo.finish_question({'questionId': future, 'id': str(uuid.uuid4()), 'text': 'Будущий ответ'})
        self.assertEqual(self.repo.question_history({'questionId': current, 'chatId': a}),
                         {'pairs': [{'question': 'Код проекта: AZ-781', 'answer': 'Код запомнен'}]})
        with self.assertRaises(ValueError):
            self.repo.question_history({'questionId': current, 'chatId': b})

    def test_background_loading_does_not_change_selected_chat(self):
        a, b = self.chat(), self.chat()
        self.repo.select_chat({'chatId': b})
        self.repo.load_chat({'chatId': a, 'remember': False})
        self.assertEqual(self.repo.bootstrap({})['currentChatId'], b)

    def test_delete_cascades_and_keeps_shared_images_and_original_files(self):
        a, b = self.chat(), self.chat()
        first, entries = self.document()
        second, second_entries = self.document()  # Same bytes => same stored image.
        self.repo.save_document({'chatId': a, 'document': copy.deepcopy(first), 'entries': entries})
        self.repo.save_document({'chatId': b, 'document': copy.deepcopy(second), 'entries': second_entries})
        stored = Path(self.repo.load_chat({'chatId': a})['documents'][0]['blocks'][0]['imagePath'])
        q = str(uuid.uuid4())
        self.repo.save_question({'id': q, 'chatId': a, 'text': 'Вопрос', 'title': 'Вопрос'})
        self.repo.finish_question({'questionId': q, 'id': str(uuid.uuid4()), 'text': 'Ответ'})
        self.repo.select_chat({'chatId': b})
        result = self.repo.dispatch('delete_chat', {'chatId': a})
        self.assertEqual(result['currentChatId'], b)
        self.assertIsNone(result['replacementChat'])
        self.assertEqual(result['warnings'], [])
        for table in ('messages', 'documents'):
            self.assertEqual(self.repo.connection.execute(f'SELECT count(*) AS n FROM {table} WHERE chat_id=%s', (a,)).fetchone()['n'], 0)
        for table in ('document_blocks', 'chunks'):
            self.assertEqual(self.repo.connection.execute(f'SELECT count(*) AS n FROM {table} WHERE document_id=%s', (first['id'],)).fetchone()['n'], 0)
        self.assertTrue(stored.is_file())
        self.assertEqual(len(self.repo.load_chat({'chatId': b})['entries']), 1)
        self.repo.delete_chat({'chatId': b})
        self.assertFalse(stored.exists())
        self.assertTrue(Path(first['sourcePath']).is_file())
        self.assertTrue(Path(second['sourcePath']).is_file())

    def test_delete_last_chat_creates_and_persists_empty_replacement(self):
        chat = self.chat()
        result = self.repo.delete_chat({'chatId': chat})
        fresh = str(result['replacementChat']['id'])
        self.assertNotEqual(chat, fresh)
        self.assertEqual(result['currentChatId'], fresh)
        restored = self.repo.bootstrap({})
        self.assertEqual(restored['currentChatId'], fresh)
        self.assertEqual(len(restored['chats']), 1)
        self.assertEqual(restored['chats'][0]['messages'], [])
        with self.assertRaisesRegex(ValueError, 'не найден'):
            self.repo.delete_chat({'chatId': chat})
        self.assertEqual(len(self.repo.bootstrap({})['chats']), 1)

    def test_delete_busy_chat_rejected_without_losing_documents(self):
        chat = self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(document), 'entries': entries})
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat, 'text': 'Ожидание', 'title': 'Ожидание'})
        with self.assertRaisesRegex(ValueError, 'завершения'):
            self.repo.delete_chat({'chatId': chat})
        self.assertEqual(len(self.repo.load_chat({'chatId': chat})['documents']), 1)
        self.assertEqual(len(self.repo.bootstrap({})['chats']), 1)

    def test_failed_file_cleanup_reports_committed_deletion_and_preserves_original(self):
        chat = self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(document), 'entries': entries})
        with patch.object(Path, 'unlink', side_effect=PermissionError('locked fixture')):
            result = self.repo.delete_chat({'chatId': chat})
        self.assertEqual(len(result['warnings']), 1)
        self.assertTrue(Path(document['sourcePath']).exists())
        self.assertNotIn(chat, [str(c['id']) for c in self.repo.bootstrap({})['chats']])

    def test_malformed_asset_path_never_deletes_external_file(self):
        chat = self.chat()
        document, entries = self.document()
        self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(document), 'entries': entries})
        self.repo.connection.execute(
            "UPDATE document_blocks SET payload=jsonb_set(payload,'{imagePath}',to_jsonb(%s::text)) WHERE document_id=%s",
            (document['sourcePath'], document['id']))
        result = self.repo.delete_chat({'chatId': chat})
        self.assertTrue(result['warnings'])
        self.assertTrue(Path(document['sourcePath']).is_file())


if __name__ == '__main__':
    unittest.main()
