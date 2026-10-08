"""Integration tests use a new private PostgreSQL cluster, never an existing database."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import uuid

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


if __name__ == '__main__':
    unittest.main()
