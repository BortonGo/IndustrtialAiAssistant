"""Integration tests use a new private PostgreSQL cluster, never an existing database."""
import copy
import base64
import csv
import io
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
        directory = ROOT / 'build-qtcreator/test-artifacts/storage'
        directory.mkdir(parents=True, exist_ok=True)
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

    def test_compare_stored_documents_without_originals_and_without_writes(self):
        chat = self.chat()
        old, entries = self.document()
        new, new_entries = self.document()
        new['blocks'][0]['text'] = 'PUMP-417: 8.0 bar'
        new['text'] = 'PUMP-417: 8.0 bar'
        new_entries[0]['chunk']['text'] = new['text']
        for doc, vectors in [(old, entries), (new, new_entries)]:
            self.repo.save_document({'chatId': chat, 'document': copy.deepcopy(doc), 'entries': vectors})
            Path(doc['sourcePath']).unlink()
        original = self.repo.load_chat({'chatId': chat})
        report = self.repo.dispatch('compare_documents', {'chatId': chat, 'documentIds': [old['id'], new['id']]})
        self.assertEqual(report['counts'], dict(added=0, removed=0, changed=1))
        change = report['changes'][0]
        self.assertEqual(change['before']['documentId'], old['id'])
        self.assertEqual(change['after']['documentId'], new['id'])
        self.assertTrue(Path(change['before']['imagePath']).is_file())
        self.assertIn('7.5 bar', change['before']['text'])
        self.assertIn('8.0 bar', change['after']['text'])
        self.assertEqual(self.repo.load_chat({'chatId': chat}), original)
        self.assertEqual(self.repo.connection.execute('SELECT count(*) AS n FROM messages').fetchone()['n'], 0)
        # Early native-text snapshots and missing stored images cannot yield a complete report.
        self.repo.connection.execute('UPDATE documents SET pending_images=3 WHERE id=%s', (new['id'],))
        partial = self.repo.compare_documents({'chatId': chat, 'documentIds': [old['id'], new['id']]})
        self.assertTrue(partial['partial'])
        self.assertTrue(any('3 изображений' in warning for warning in partial['warnings']))
        self.repo.connection.execute('UPDATE documents SET pending_images=0 WHERE id=%s', (new['id'],))
        Path(change['before']['imagePath']).unlink()
        missing = self.repo.compare_documents({'chatId': chat, 'documentIds': [old['id'], new['id']]})
        self.assertTrue(missing['partial'])
        self.assertTrue(any('недоступно' in warning for warning in missing['warnings']))

    def test_ocr_provenance_survives_storage_reload_and_comparison(self):
        chat = self.chat()
        ids = []
        for text in ('Давление 7,5 бар', 'Давление 8,0 бар'):
            doc, entries = self.document()
            doc['blocks'][0].update(text=text, recognitionMethod='windows-ocr')
            doc['text'] = '[Текст изображения распознан OCR; возможны ошибки]\n' + text
            entries[0]['chunk']['text'] = doc['text']
            self.repo.save_document(dict(chatId=chat, document=copy.deepcopy(doc), entries=entries))
            ids.append(doc['id'])
            Path(doc['sourcePath']).unlink()
        loaded = self.repo.load_chat(dict(chatId=chat))
        for doc in loaded['documents']:
            self.assertEqual(doc['blocks'][0]['recognitionMethod'], 'windows-ocr')
            self.assertTrue(Path(doc['blocks'][0]['imagePath']).is_file())
        report = self.repo.compare_documents(dict(chatId=chat, documentIds=ids))
        before = report['changes'][0]['before']['text']
        self.assertIn('распознан OCR', before)
        self.assertNotIn('моделью', before)

    def test_comparison_rejects_wrong_chat_missing_and_duplicate_ids(self):
        a, b = self.chat(), self.chat()
        old, entries = self.document()
        new, new_entries = self.document()
        self.repo.save_document({'chatId': a, 'document': old, 'entries': entries})
        self.repo.save_document({'chatId': b, 'document': new, 'entries': new_entries})
        for ids in [[], [old['id']], [old['id'], old['id']], [old['id'], new['id']],
                    [old['id'], str(uuid.uuid4())], [old['id']] * 3]:
            with self.subTest(ids=ids), self.assertRaises(ValueError):
                self.repo.compare_documents({'chatId': a, 'documentIds': ids})
        # A failed read-only transaction must not poison subsequent worker operations.
        self.assertEqual(len(self.repo.load_chat({'chatId': a})['documents']), 1)

    def test_table_export_uses_same_snapshot_isolated_from_other_chats(self):
        a, b = self.chat(), self.chat()
        document, entries = self.document()
        document['text'] = 'Код\tДавление\n00017\t7,5 bar'
        rows = [['Код', 'Давление'], ['00017', '7,5 bar']]
        document['blocks'] = [dict(type='table', tableRows=rows, pageNumber=4, tableIndex=0)]
        entries[0]['chunk'] = dict(documentId=document['id'], text=document['text'], startOffset=0)
        self.repo.save_document(dict(chatId=a, document=document, entries=entries))
        before = self.repo.load_chat(dict(chatId=a))
        params = dict(chatId=a, documentId=document['id'])
        report = self.repo.dispatch('document_tables', params)
        self.assertEqual(report['tables'][0]['rows'], rows)
        request = dict(params, snapshot=report['snapshot'], tableId='0:0', format='csv')
        exported = self.repo.dispatch('export_document_table', request)
        data = base64.b64decode(exported['data']).decode('utf-8-sig')
        self.assertEqual(list(csv.reader(io.StringIO(data), delimiter=';')), rows)
        self.assertEqual(before, self.repo.load_chat(dict(chatId=a)))
        with self.assertRaises(ValueError): self.repo.document_tables(dict(params, chatId=b))
        with self.assertRaises(ValueError): self.repo.export_document_table(dict(request, chatId=b))
        with self.assertRaises(ValueError): self.repo.export_document_table(dict(request, snapshot='stale'))
        with self.assertRaises(ValueError): self.repo.export_document_table(dict(request, tableId='99:0'))
        with self.assertRaises(ValueError): self.repo.export_document_table(dict(request, format='html'))
        # Reindexing between preview and save must never export a different value.
        document['blocks'][0]['tableRows'][1][1] = '8,0 bar'
        document['text'] = 'Код\tДавление\n00017\t8,0 bar'; entries[0]['chunk']['text'] = document['text']
        self.repo.save_document(dict(chatId=a, document=document, entries=entries))
        with self.assertRaisesRegex(ValueError, 'изменились'): self.repo.export_document_table(request)
        report = self.repo.document_tables(params)
        Path(document['sourcePath']).unlink()
        xlsx = self.repo.export_document_table(dict(request, snapshot=report['snapshot'], format='xlsx'))
        self.assertTrue(base64.b64decode(xlsx['data']).startswith(b'PK'))

    def test_table_export_preserves_partial_and_recognition_provenance(self):
        chat = self.chat(); doc, entries = self.document()
        doc['blocks'][0]['text'] = '| Код | Давление |\n|---|---|\n| 00017 | 7,5 bar |'
        doc['text'] = doc['blocks'][0]['text']; entries[0]['chunk']['text'] = doc['text']
        self.repo.save_document(dict(chatId=chat, document=doc, entries=entries))
        self.repo.connection.execute('UPDATE documents SET pending_images=12 WHERE id=%s', (doc['id'],))
        report = self.repo.document_tables(dict(chatId=chat, documentId=doc['id']))
        self.assertTrue(report['partial'])
        self.assertEqual(report['tables'][0]['origin'], 'recognition')
        self.assertTrue(Path(report['tables'][0]['reference']['imagePath']).is_file())
        self.assertTrue(any('12' in w for w in report['warnings']))

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

    def test_source_snapshots_survive_reindex_and_restart(self):
        chat_id, question = self.chat(), str(uuid.uuid4())
        document, entries = self.document()
        self.repo.save_document({'chatId': chat_id, 'document': document, 'entries': entries})
        image = self.repo.load_chat({'chatId': chat_id})['documents'][0]['blocks'][0]['imagePath']
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Давление?', 'title': 'Источники'})
        ref = dict(documentId=document['id'], sourcePath='ignored', text='Old value: 7.5 bar', imagePath=image,
                   pageFirst=4, pageLast=4, tableIndices=[0], blockFirst=0, blockLast=1)
        saved = self.repo.save_context({'id': question, 'context': 'Old evidence', 'references': [ref]})['references']
        self.assertEqual(saved[0]['sourcePath'], document['sourcePath'])
        # Newly attached images can still live in the extractor cache in the C++ workspace.
        ref['imagePath'] = document['sourcePath']
        saved_from_cache = self.repo.save_context({'id': question, 'context': 'Old evidence', 'references': [ref]})['references']
        self.assertEqual(saved_from_cache, saved)
        self.repo.finish_question({'questionId': question, 'id': str(uuid.uuid4()), 'text': '7.5 bar'})
        document['text'] = 'New value: 12 bar'
        document['blocks'] = [{'type': 'text', 'text': document['text'], 'pageNumber': 0}]
        entries[0]['chunk'] = dict(documentId=document['id'], text=document['text'], startOffset=0)
        self.repo.save_document({'chatId': chat_id, 'document': document, 'entries': entries})
        Path(document['sourcePath']).unlink()
        restarted = Repository(self.options, self.work / 'data', self.model)
        try:
            message = restarted.bootstrap({})['chats'][0]['messages'][0]
            self.assertEqual(message['source_references'], saved)
            self.assertEqual(message['context_snapshot'], 'Old evidence')
            self.assertTrue(Path(saved[0]['imagePath']).is_file())
        finally:
            restarted.close()

    def test_sources_reject_cross_chat_and_invalid_locations(self):
        a, b, question = self.chat(), self.chat(), str(uuid.uuid4())
        document, entries = self.document()
        self.repo.save_document({'chatId': a, 'document': document, 'entries': entries})
        self.repo.save_question({'id': question, 'chatId': b, 'text': 'Q', 'title': 'Q'})
        ref = dict(documentId=document['id'], text='Evidence')
        with self.assertRaises(ValueError):
            self.repo.save_context({'id': question, 'context': 'foreign', 'references': [ref]})
        self.repo.finish_question({'questionId': question, 'error': 'fixture'})
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': a, 'text': 'Q', 'title': 'Q'})
        self.repo.save_context({'id': question, 'context': 'unchanged'})
        for changes in ({'pageFirst': -1}, {'pageFirst': True}, {'pageFirst': 4, 'pageLast': 2},
                        {'tableIndices': [-1]}, {'section': []}, {'imagePath': str(self.model)}):
            with self.assertRaises(ValueError):
                self.repo.save_context({'id': question, 'context': 'invalid', 'references': [dict(ref, **changes)]})
        row = self.repo.connection.execute('SELECT context_snapshot,source_references FROM messages WHERE id=%s', (question,)).fetchone()
        self.assertEqual(row, {'context_snapshot': 'unchanged', 'source_references': []})

    def test_source_image_lifetime_after_reindex_and_chat_deletion(self):
        a, b, question = self.chat(), self.chat(), str(uuid.uuid4())
        document, entries = self.document()
        self.repo.save_document({'chatId': a, 'document': copy.deepcopy(document), 'entries': copy.deepcopy(entries)})
        other = copy.deepcopy(document); other['id'] = str(uuid.uuid4())
        other_entries = copy.deepcopy(entries); other_entries[0]['chunk']['documentId'] = other['id']
        self.repo.save_document({'chatId': b, 'document': other, 'entries': other_entries})
        image = self.repo.load_chat({'chatId': a})['documents'][0]['blocks'][0]['imagePath']
        self.repo.save_question({'id': question, 'chatId': a, 'text': 'Q', 'title': 'Q'})
        self.repo.save_context({'id': question, 'context': 'Saved image', 'references': [dict(documentId=document['id'], text='Old image', imagePath=image)]})
        self.repo.finish_question({'questionId': question, 'id': str(uuid.uuid4()), 'text': 'A'})
        document['blocks'] = [{'type': 'text', 'text': 'new', 'pageNumber': 0}]; document['text'] = 'new'
        entries[0]['chunk'] = dict(documentId=document['id'], text='new', startOffset=0)
        self.repo.save_document({'chatId': a, 'document': document, 'entries': entries})
        self.repo.delete_chat({'chatId': b})
        self.assertTrue(Path(image).is_file(), 'Another chat deleted an image used only by an old answer')
        self.repo.delete_chat({'chatId': a})
        self.assertFalse(Path(image).exists(), 'Last snapshot did not release its image')
        self.assertTrue(Path(document['sourcePath']).is_file())

    def test_sources_migration_keeps_legacy_context(self):
        chat_id, question = self.chat(), str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': chat_id, 'text': 'Old Q', 'title': 'Old'})
        self.repo.save_context({'id': question, 'context': 'Legacy evidence'})
        with self.repo.connection.transaction():
            self.repo.connection.execute('ALTER TABLE messages DROP COLUMN source_references')
            self.repo.connection.execute('DELETE FROM schema_migrations WHERE version=6')
        self.repo.migrate(); self.repo.migrate()
        message = self.repo.bootstrap({})['chats'][0]['messages'][0]
        self.assertEqual(message['source_references'], [])
        self.assertEqual(message['context_snapshot'], 'Legacy evidence')

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


    def import_job(self, chat_id=None):
        document, entries = self.document()
        chat_id = chat_id or self.chat()
        job = self.repo.enqueue_import({'id': str(uuid.uuid4()), 'chatId': chat_id, 'sourcePath': document['sourcePath']})['job']
        document['id'] = job['documentId']
        entries[0]['chunk']['documentId'] = job['documentId']
        return job, document, entries

    def test_text_preview_survives_pause_restart_and_is_replaced_atomically(self):
        job, full, entries = self.import_job()
        preview = copy.deepcopy(full)
        preview.update(text='Native fact', blocks=[{'type':'text', 'text':'Native fact'}], pendingImages=2)
        native_entries = copy.deepcopy(entries)
        native_entries[0]['chunk'].update(text='Native fact', imagePath='')
        self.repo.start_import({'id': job['id']})
        self.repo.save_document({'importId':job['id'], 'chatId':job['chatId'], 'document':preview, 'entries':native_entries})
        self.assertEqual(self.repo.list_imports({})['jobs'][0]['status'], 'running')
        restored = self.repo.load_chat({'chatId':job['chatId']})
        self.assertEqual(restored['documents'][0]['status'], 'TextReady')
        self.assertEqual(restored['documents'][0]['pendingImages'], 2)
        self.assertEqual(restored['entries'][0]['chunk']['text'], 'Native fact')
        self.repo.close()
        self.repo = Repository(self.options, self.work / 'data', self.model)
        self.assertEqual(self.repo.list_imports({})['jobs'][0]['status'], 'paused')
        self.repo.update_import({'id':job['id'], 'status':'queued'})
        resumed = self.repo.start_import({'id':job['id']})
        self.assertTrue(resumed['job']['nativeDone'])
        self.assertEqual(resumed['job']['checkpoint'], {})
        self.assertEqual(resumed['reusableEntries'][0]['text'], 'Native fact')
        bad = copy.deepcopy(entries); bad[0]['embedding'] = [1.0]
        with self.assertRaises(ValueError):
            self.repo.save_document({'importId':job['id'], 'chatId':job['chatId'], 'document':copy.deepcopy(full), 'entries':bad})
        self.assertEqual(self.repo.load_chat({'chatId':job['chatId']})['documents'][0]['status'], 'TextReady')
        self.repo.save_document({'importId':job['id'], 'chatId':job['chatId'], 'document':full, 'entries':entries})
        complete = self.repo.load_chat({'chatId':job['chatId']})
        self.assertEqual(len(complete['documents']), 1)
        self.assertEqual(complete['documents'][0]['status'], 'Ready')
        self.assertEqual(complete['documents'][0]['pendingImages'], 0)
        self.assertEqual(complete['documents'][0]['blocks'][0]['type'], 'image')

    def test_native_preview_never_replaces_existing_document_and_cancel_keeps_preview(self):
        job, full, entries = self.import_job()
        self.repo.save_document({'chatId':job['chatId'], 'document':copy.deepcopy(full), 'entries':copy.deepcopy(entries)})
        self.assertTrue(self.repo.start_import({'id':job['id']})['job']['nativeDone'])
        preview = copy.deepcopy(full)
        preview.update(text='Native fact', blocks=[{'type':'text', 'text':'Native fact'}], pendingImages=1)
        native_entries = copy.deepcopy(entries); native_entries[0]['chunk'].update(text='Native fact', imagePath='')
        result = self.repo.save_document({'importId':job['id'], 'chatId':job['chatId'], 'document':preview, 'entries':native_entries})
        self.assertFalse(result['published'])
        self.assertEqual(self.repo.load_chat({'chatId':job['chatId']})['documents'][0]['status'], 'Ready')
        other, doc, values = self.import_job()
        self.repo.start_import({'id':other['id']})
        doc.update(text='Preview', blocks=[{'type':'text', 'text':'Preview'}], pendingImages=1)
        values[0]['chunk'].update(text='Preview', imagePath='')
        self.repo.save_document({'importId':other['id'], 'chatId':other['chatId'], 'document':doc, 'entries':values})
        self.repo.update_import({'id':other['id'], 'status':'cancelled'})
        self.assertEqual(self.repo.load_chat({'chatId':other['chatId']})['documents'][0]['status'], 'TextReady')

    def test_import_deduplicates_and_survives_restart_with_checkpoint(self):
        job, document, entries = self.import_job()
        duplicate = self.repo.enqueue_import({'id': str(uuid.uuid4()), 'chatId': job['chatId'], 'sourcePath': job['sourcePath']})['job']
        self.assertEqual(job['id'], duplicate['id'])
        self.repo.start_import({'id': job['id']})
        self.repo.checkpoint_import({'id': job['id'], 'document': document, 'progress': 'extracted'})
        self.repo.checkpoint_import({'id': job['id'], 'entries': entries, 'offset': 0, 'progress': '1/2'})
        # Simulate a new application opening the same private cluster after interruption.
        self.repo.close()
        type(self).repo = Repository(self.options, self.work / 'data', self.model)
        restored = self.repo.list_imports({})['jobs'][0]
        self.assertEqual(restored['status'], 'paused')
        self.repo.update_import({'id': job['id'], 'status': 'queued'})
        resumed = self.repo.start_import({'id': job['id']})['job']
        self.assertEqual(resumed['checkpoint']['document'], document)
        self.assertEqual(resumed['checkpoint']['entries'], entries)
        with self.assertRaisesRegex(ValueError, 'порядок'):
            self.repo.checkpoint_import({'id': job['id'], 'entries': entries, 'offset': 0, 'progress': 'duplicate'})

    def test_import_completion_atomically_replaces_document_and_clears_checkpoint(self):
        job, document, entries = self.import_job()
        self.repo.save_document({'chatId': job['chatId'], 'document': copy.deepcopy(document), 'entries': entries})
        self.repo.start_import({'id': job['id']})
        self.repo.checkpoint_import({'id': job['id'], 'document': document, 'progress': 'extracted'})
        self.repo.checkpoint_import({'id': job['id'], 'entries': entries, 'offset': 0, 'progress': 'saved'})
        changed = copy.deepcopy(document)
        changed['text'] = 'Changed result'
        payload = {'importId': job['id'], 'chatId': job['chatId'], 'document': changed, 'entries': entries}
        bad = copy.deepcopy(payload)
        bad['document']['blocks'][0]['type'] = 'invalid'
        with self.assertRaises(Exception):
            self.repo.save_document(bad)
        self.assertEqual(self.repo.load_chat({'chatId': job['chatId']})['documents'][0]['text'], document['text'])
        self.assertEqual(self.repo.list_imports({})['jobs'][0]['status'], 'running')
        self.repo.save_document(payload)
        self.assertEqual(self.repo.list_imports({})['jobs'], [])
        self.assertEqual(self.repo.connection.execute('SELECT count(*) AS n FROM document_import_chunks').fetchone()['n'], 0)
        self.assertEqual(self.repo.load_chat({'chatId': job['chatId']})['documents'][0]['text'], 'Changed result')

    def test_import_source_edit_and_chat_mismatch_cannot_publish(self):
        job, document, entries = self.import_job()
        self.repo.start_import({'id': job['id']})
        with self.assertRaisesRegex(ValueError, 'Задание'):
            self.repo.save_document({'importId': job['id'], 'chatId': self.chat(), 'document': copy.deepcopy(document), 'entries': entries})
        Path(job['sourcePath']).write_bytes(b'changed image')
        with self.assertRaisesRegex(ValueError, 'Задание'):
            self.repo.save_document({'importId': job['id'], 'chatId': job['chatId'], 'document': copy.deepcopy(document), 'entries': entries})
        self.repo.update_import({'id': job['id'], 'status': 'queued'})
        self.assertEqual(self.repo.start_import({'id': job['id']})['job']['status'], 'failed')
        self.assertEqual(self.repo.load_chat({'chatId': job['chatId']})['documents'], [])

    def test_foreground_attachment_reference_survives_queued_import_completion(self):
        job, document, entries = self.import_job()
        foreground = copy.deepcopy(document)
        foreground['id'] = str(uuid.uuid4())
        foreground_entries = copy.deepcopy(entries)
        foreground_entries[0]['chunk']['documentId'] = foreground['id']
        self.repo.save_document({'chatId': job['chatId'], 'document': foreground, 'entries': foreground_entries})
        question = str(uuid.uuid4())
        self.repo.save_question({'id': question, 'chatId': job['chatId'], 'text': 'Photo', 'title': 'Photo', 'attachments': [job['sourcePath']]})
        self.repo.bind_attachment({'questionId': question, 'position': 0, 'documentId': foreground['id']})
        self.repo.start_import({'id': job['id']})
        self.repo.save_document({'importId': job['id'], 'chatId': job['chatId'], 'document': document, 'entries': entries})
        stored = self.repo.load_chat({'chatId': job['chatId']})
        self.assertEqual(stored['documents'][0]['id'], foreground['id'])
        self.assertEqual(stored['entries'][0]['chunk']['documentId'], foreground['id'])
        row = self.repo.connection.execute('SELECT document_id FROM message_attachments WHERE message_id=%s', (question,)).fetchone()
        self.assertEqual(str(row['document_id']), foreground['id'])

    def test_import_pause_cancel_and_chat_delete_are_isolated(self):
        job, document, entries = self.import_job()
        other, _, _ = self.import_job()
        self.repo.start_import({'id': job['id']})
        with self.assertRaisesRegex(ValueError, 'приостановите'):
            self.repo.delete_chat({'chatId': job['chatId']})
        self.repo.checkpoint_import({'id': job['id'], 'document': document, 'progress': 'text'})
        self.repo.checkpoint_import({'id': job['id'], 'entries': entries, 'offset': 0, 'progress': '1'})
        self.repo.update_import({'id': job['id'], 'status': 'paused'})
        self.repo.delete_chat({'chatId': job['chatId']})
        self.assertEqual([j['id'] for j in self.repo.list_imports({})['jobs']], [other['id']])
        self.assertEqual(self.repo.connection.execute('SELECT count(*) AS n FROM document_import_chunks').fetchone()['n'], 0)
        self.assertTrue(Path(job['sourcePath']).exists())
        self.repo.update_import({'id': other['id'], 'status': 'cancelled'})
        self.assertEqual(self.repo.list_imports({})['jobs'], [])


if __name__ == '__main__':
    unittest.main()
