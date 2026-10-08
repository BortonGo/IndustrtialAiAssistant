"""Transactional repositories. Each method is one short transaction, without LLM calls."""
import hashlib
import json
import math
import os
import re
from pathlib import Path
import uuid

import psycopg
from psycopg.rows import dict_row
from psycopg.types.json import Jsonb

PIPELINE_VERSION = "blocks-v2-image-v2-chunks500-overlap100-utf16"
PREPROCESSING = "embeddinggemma-raw-text-v1"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Repository:
    def __init__(self, options, data: Path, model: Path, dimension=768):
        self.connection = psycopg.connect(**options, autocommit=True, row_factory=dict_row)
        self.connection.execute("SET statement_timeout = '30s'")
        self.connection.execute("SET lock_timeout = '5s'")
        self.data = data
        self.dimension = dimension
        model_hash = sha256(model)
        self.profile = hashlib.sha256(f"{model_hash}:{dimension}:{PREPROCESSING}".encode()).hexdigest()
        self.migrate()
        with self.connection.transaction():
            self.connection.execute("INSERT INTO embedding_profiles VALUES (%s,%s,%s,%s) ON CONFLICT DO NOTHING",
                                    (self.profile, model_hash, PREPROCESSING, dimension))
            self.connection.execute("UPDATE messages SET status='failed', error='Запрос прерван при завершении приложения' WHERE status='pending'")

    def migrate(self):
        with self.connection.transaction():
            self.connection.execute("SELECT pg_advisory_xact_lock(170011)")
            self.connection.execute("CREATE TABLE IF NOT EXISTS schema_migrations(version integer PRIMARY KEY, checksum text NOT NULL, applied_at timestamptz NOT NULL DEFAULT now())")
            applied = {row['version']: row['checksum'] for row in self.connection.execute("SELECT version,checksum FROM schema_migrations")}
            migrations = sorted((Path(__file__).parent / "migrations").glob("*.sql"))
            if set(applied) - {int(path.name.split('_')[0]) for path in migrations}:
                raise ValueError("База создана более новой версией приложения")
            for path in migrations:
                version = int(path.name.split('_')[0])
                checksum = sha256(path)
                if version in applied:
                    if applied[version] != checksum:
                        raise ValueError("Применённая SQL-миграция изменена: " + path.name)
                    continue
                self.connection.execute(path.read_text(encoding="utf-8"))
                self.connection.execute("INSERT INTO schema_migrations(version,checksum) VALUES(%s,%s)", (version, checksum))

    def bootstrap(self, params):
        with self.connection.transaction():
            if not self.connection.execute("SELECT 1 FROM chats LIMIT 1").fetchone():
                self.connection.execute("INSERT INTO chats(id,title) VALUES(%s,%s)", (uuid.uuid4(), "Новый чат"))
            chats = list(self.connection.execute("SELECT * FROM chats ORDER BY created_at,id"))
            for chat in chats:
                chat["messages"] = list(self.connection.execute("SELECT * FROM messages WHERE chat_id=%s ORDER BY position", (chat['id'],)))
                attachments = {}
                for row in self.connection.execute("SELECT a.* FROM message_attachments a JOIN messages m ON m.id=a.message_id WHERE m.chat_id=%s ORDER BY a.position", (chat['id'],)):
                    attachments.setdefault(row['message_id'], []).append(self.attachment_json(row))
                for message in chat['messages']:
                    message['attachments'] = attachments.get(message['id'], [])
            current = self.connection.execute("SELECT value FROM app_settings WHERE key='current_chat'").fetchone()
            current_id = current['value'] if current else str(chats[-1]['id'])
            if current_id not in {str(c['id']) for c in chats}:
                current_id = str(chats[-1]['id'])
            return {"chats": chats, "currentChatId": current_id, "profile": self.profile}

    def create_chat(self, params):
        with self.connection.transaction():
            return self.connection.execute("INSERT INTO chats(id,title) VALUES(%s,%s) RETURNING *",
                                           (params['id'], params['title'])).fetchone()

    def save_question(self, params):
        attachments = params.get('attachments', [])
        if not isinstance(attachments, list) or len(attachments) > 4 or any(not isinstance(path, str) or not path for path in attachments):
            raise ValueError('Можно прикрепить до четырёх файлов к сообщению')
        with self.connection.transaction():
            self.connection.execute("SELECT id FROM chats WHERE id=%s FOR UPDATE", (params['chatId'],))
            self.connection.execute("INSERT INTO messages(id,chat_id,role,text,status) VALUES(%s,%s,'user',%s,'pending')",
                                    (params['id'], params['chatId'], params['text']))
            for position, path in enumerate(attachments):
                self.connection.execute('INSERT INTO message_attachments(message_id,position,file_name,source_path) VALUES(%s,%s,%s,%s)',
                                        (params['id'], position, Path(path).name, path))
            self.connection.execute("UPDATE chats SET title=CASE WHEN (SELECT count(*) FROM messages WHERE chat_id=%s)=1 THEN %s ELSE title END, updated_at=now() WHERE id=%s",
                                    (params['chatId'], params['title'], params['chatId']))
        return {}

    def attachment_json(self, row):
        return {'fileName': row['file_name'], 'sourcePath': row['source_path'],
                'documentId': str(row['document_id']) if row['document_id'] else '',
                'previewPath': str(self.data / 'images' / Path(row['preview_image']).name) if row['preview_image'] else ''}

    def bind_attachment(self, params):
        with self.connection.transaction():
            question = self.connection.execute("SELECT chat_id FROM messages WHERE id=%s AND role='user' AND status='pending' FOR UPDATE", (params['questionId'],)).fetchone()
            document = self.connection.execute('SELECT chat_id,source_path FROM documents WHERE id=%s', (params['documentId'],)).fetchone()
            attachment = self.connection.execute('SELECT * FROM message_attachments WHERE message_id=%s AND position=%s', (params['questionId'], params['position'])).fetchone()
            if not question or not document or not attachment or question['chat_id'] != document['chat_id']:
                raise ValueError('Вложение и документ должны принадлежать исходному чату')
            if os.path.normcase(os.path.abspath(attachment['source_path'])) != os.path.normcase(os.path.abspath(document['source_path'])):
                raise ValueError('Документ не соответствует прикреплённому файлу')
            preview = ''
            if Path(attachment['source_path']).suffix.lower() in ('.png', '.jpg', '.jpeg', '.webp', '.bmp'):
                image = self.connection.execute("SELECT payload->>'imagePath' AS path FROM document_blocks WHERE document_id=%s AND kind='image' ORDER BY position LIMIT 1", (params['documentId'],)).fetchone()
                if image:
                    preview = image['path']
            self.connection.execute('UPDATE message_attachments SET document_id=%s,preview_image=%s WHERE message_id=%s AND position=%s',
                                    (params['documentId'], preview, params['questionId'], params['position']))
            return {'attachments': [self.attachment_json(row) for row in self.connection.execute('SELECT * FROM message_attachments WHERE message_id=%s ORDER BY position', (params['questionId'],))]}

    def delete_chat(self, params):
        chat_id = params['chatId']
        with self.connection.transaction():
            if not self.connection.execute("SELECT id FROM chats WHERE id=%s FOR UPDATE", (chat_id,)).fetchone():
                raise ValueError("Чат уже удалён или не найден")
            if self.connection.execute("SELECT 1 FROM messages WHERE chat_id=%s AND status='pending' LIMIT 1", (chat_id,)).fetchone():
                raise ValueError("Дождитесь завершения ответа перед удалением чата")
            images = {row['path'] for row in self.connection.execute(
                "SELECT b.payload->>'imagePath' AS path FROM document_blocks b "
                "JOIN documents d ON d.id=b.document_id WHERE d.chat_id=%s AND b.kind='image'", (chat_id,))}
            images.update(row['preview_image'] for row in self.connection.execute(
                "SELECT a.preview_image FROM message_attachments a JOIN messages m ON m.id=a.message_id WHERE m.chat_id=%s AND a.preview_image<>''", (chat_id,)))
            self.connection.execute("DELETE FROM chats WHERE id=%s", (chat_id,))
            remaining = list(self.connection.execute("SELECT * FROM chats ORDER BY created_at,id"))
            replacement = None
            if not remaining:
                replacement = self.create_chat({'id': str(uuid.uuid4()), 'title': 'Новый чат'})
                remaining.append(replacement)
            current = self.connection.execute("SELECT value FROM app_settings WHERE key='current_chat'").fetchone()
            selected = params.get('currentChatId') or (current['value'] if current else '')
            if selected not in {str(chat['id']) for chat in remaining}:
                selected = str(remaining[-1]['id'])
            self.select_chat({'chatId': selected})

        # DB deletion has committed. Files shared with another chat must survive.
        # The worker serializes requests, so no save can interleave this cleanup.
        warnings = []
        assets = self.data / 'images'
        for name in images:
            if not isinstance(name, str) or not re.fullmatch(r'[0-9a-f]{64}\.png', name):
                warnings.append('Пропущено изображение с некорректным именем')
                continue
            try:
                if self.connection.execute(
                        "SELECT 1 FROM document_blocks WHERE payload->>'imagePath'=%s "
                        "UNION ALL SELECT 1 FROM chunks WHERE metadata->>'imagePath'=%s "
                        "UNION ALL SELECT 1 FROM message_attachments WHERE preview_image=%s LIMIT 1", (name, name, name)).fetchone():
                    continue
                target = assets / name
                if assets.is_symlink() or assets.resolve() != self.data.resolve() / 'images' or target.is_symlink():
                    raise OSError('Путь изображения выходит за папку хранения')
                target.unlink(missing_ok=True)
            except (OSError, psycopg.Error) as error:
                warnings.append(f'Не удалось удалить сохранённую копию {name}: {error}')
        return {'deletedChatId': chat_id, 'currentChatId': selected, 'replacementChat': replacement, 'warnings': warnings}

    def save_context(self, params):
        with self.connection.transaction():
            row = self.connection.execute("UPDATE messages SET context_snapshot=%s WHERE id=%s AND role='user' AND status='pending' RETURNING id",
                                          (params['context'], params['id'])).fetchone()
            if not row:
                raise ValueError("Вопрос для сохранения источников не найден")
        return {}

    def question_history(self, params):
        question = self.connection.execute(
            "SELECT chat_id,position FROM messages WHERE id=%s AND role='user' AND status='pending'",
            (params['questionId'],)).fetchone()
        if not question or str(question['chat_id']) != params['chatId']:
            raise ValueError("Вопрос не принадлежит указанному чату или уже завершён")
        pairs = list(self.connection.execute(
            "SELECT q.text AS question,a.text AS answer,q.id AS question_id FROM messages q "
            "JOIN messages a ON a.reply_to=q.id AND a.chat_id=q.chat_id "
            "WHERE q.chat_id=%s AND q.position<%s AND a.position<%s "
            "AND q.role='user' AND a.role='assistant' AND q.status='complete' AND a.status='complete' "
            "ORDER BY q.position", (question['chat_id'], question['position'], question['position'])))
        attached_names = {}
        if pairs:
            for row in self.connection.execute('SELECT message_id,file_name FROM message_attachments WHERE message_id=ANY(%s) ORDER BY position', ([pair['question_id'] for pair in pairs],)):
                attached_names.setdefault(row['message_id'], []).append(row['file_name'])
        for pair in pairs:
            files = attached_names.get(pair.pop('question_id'), [])
            if files:
                pair['question'] += '\n[К этому сообщению были прикреплены файлы: ' + ', '.join(files) + ']'
        return {'pairs': pairs}

    def select_chat(self, params):
        if not self.connection.execute("SELECT 1 FROM chats WHERE id=%s", (params['chatId'],)).fetchone():
            raise ValueError("Чат не найден")
        self.connection.execute("INSERT INTO app_settings VALUES('current_chat',%s) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (params['chatId'],))
        return {}

    def finish_question(self, params):
        with self.connection.transaction():
            question = self.connection.execute("SELECT * FROM messages WHERE id=%s AND role='user' FOR UPDATE", (params['questionId'],)).fetchone()
            if not question:
                raise ValueError("Исходный вопрос не найден")
            # Retrying acknowledgement after a disconnect must not duplicate an answer.
            existing = self.connection.execute("SELECT id FROM messages WHERE reply_to=%s", (question['id'],)).fetchone()
            if existing:
                if str(existing['id']) != params.get('id'):
                    raise ValueError("У вопроса уже есть ответ")
                return {}
            if params.get('error'):
                self.connection.execute("UPDATE messages SET status='failed',error=%s WHERE id=%s", (params['error'], question['id']))
                if params.get('text'):
                    self.connection.execute("INSERT INTO messages(id,chat_id,role,text,status,error,reply_to) VALUES(%s,%s,'assistant',%s,'failed',%s,%s)",
                                            (params['id'], question['chat_id'], params['text'], params['error'], question['id']))
            else:
                self.connection.execute("INSERT INTO messages(id,chat_id,role,text,status,reply_to) VALUES(%s,%s,'assistant',%s,'complete',%s)",
                                        (params['id'], question['chat_id'], params['text'], question['id']))
                self.connection.execute("UPDATE messages SET status='complete',error='' WHERE id=%s", (question['id'],))
            self.connection.execute("UPDATE chats SET updated_at=now() WHERE id=%s", (question['chat_id'],))
        return {}

    def save_document(self, params):
        document = params['document']
        entries = params['entries']
        if not document['text'].strip() or not entries:
            raise ValueError("Нельзя сохранить пустой документ или частичный индекс")
        for entry in entries:
            vector = entry['embedding']
            if len(vector) != self.dimension or not all(isinstance(x, (float, int)) and not isinstance(x, bool) and math.isfinite(x) for x in vector) or not any(vector):
                raise ValueError("Некорректный embedding или несовместимая размерность")
        paths = {}
        assets = self.data / 'images'
        assets.mkdir(parents=True, exist_ok=True)
        for block in document['blocks']:
            if block['type'] == 'image':
                original = block['imagePath']
                if original not in paths:
                    content = Path(original).read_bytes()
                    target = assets / (hashlib.sha256(content).hexdigest() + '.png')
                    if not target.exists():
                        temporary = target.with_suffix('.tmp')
                        temporary.write_bytes(content)
                        temporary.replace(target)
                    paths[original] = target.name
                block['imagePath'] = paths[original]
        source = document['sourcePath']
        source_key = os.path.normcase(os.path.abspath(source))
        source_hash = sha256(source)
        with self.connection.transaction():
            owner = self.connection.execute('SELECT chat_id FROM documents WHERE id=%s FOR UPDATE', (document['id'],)).fetchone()
            if owner and str(owner['chat_id']) != params['chatId']:
                raise ValueError('Документ принадлежит другому чату')
            previous = self.connection.execute("SELECT id FROM documents WHERE chat_id=%s AND source_key=%s", (params['chatId'], source_key)).fetchone()
            if previous and str(previous['id']) != document['id']:
                self.connection.execute("DELETE FROM documents WHERE id=%s", (previous['id'],))
            if previous and str(previous['id']) == document['id']:
                self.connection.execute('DELETE FROM document_blocks WHERE document_id=%s', (document['id'],))
                self.connection.execute('DELETE FROM chunks WHERE document_id=%s', (document['id'],))
            self.connection.execute("INSERT INTO documents(id,chat_id,source_path,source_key,content_sha256,extracted_text,warnings,pipeline_version) VALUES(%s,%s,%s,%s,%s,%s,%s,%s) "
                                    "ON CONFLICT(id) DO UPDATE SET source_path=excluded.source_path,source_key=excluded.source_key,content_sha256=excluded.content_sha256,extracted_text=excluded.extracted_text,warnings=excluded.warnings,pipeline_version=excluded.pipeline_version WHERE documents.chat_id=excluded.chat_id",
                                    (document['id'], params['chatId'], source, source_key, source_hash, document['text'], Jsonb(document['warnings']), PIPELINE_VERSION))
            with self.connection.cursor() as cursor:
                cursor.executemany("INSERT INTO document_blocks VALUES(%s,%s,%s,%s)",
                                   [(document['id'], i, b['type'], Jsonb(b)) for i, b in enumerate(document['blocks'])])
                rows = []
                for i, entry in enumerate(entries):
                    metadata = entry['chunk'].copy()
                    if metadata.get('imagePath'):
                        metadata['imagePath'] = paths[metadata['imagePath']]
                    rows.append((document['id'], i, metadata.pop('text'), metadata.pop('startOffset'), Jsonb(metadata), self.profile, entry['embedding']))
                cursor.executemany("INSERT INTO chunks VALUES(%s,%s,%s,%s,%s,%s,%s)", rows)
        return {}

    def load_chat(self, params):
        with self.connection.transaction():
            if not self.connection.execute("SELECT 1 FROM chats WHERE id=%s", (params['chatId'],)).fetchone():
                raise ValueError("Чат не найден")
            result = {"documents": [], "entries": []}
            for row in self.connection.execute("SELECT * FROM documents WHERE chat_id=%s ORDER BY created_at,id", (params['chatId'],)):
                document = {"id": str(row['id']), "sourcePath": row['source_path'], "text": row['extracted_text'], "warnings": row['warnings'], "blocks": []}
                missing_images = []
                for block in self.connection.execute("SELECT payload FROM document_blocks WHERE document_id=%s ORDER BY position", (row['id'],)):
                    payload = block['payload']
                    if payload['type'] == 'image':
                        payload['imagePath'] = str(self.data / 'images' / Path(payload['imagePath']).name)
                        if not Path(payload['imagePath']).is_file():
                            missing_images.append(payload['imagePath'])
                    document['blocks'].append(payload)
                chunks = list(self.connection.execute("SELECT * FROM chunks WHERE document_id=%s AND profile_id=%s ORDER BY position", (row['id'], self.profile)))
                compatible = bool(chunks) and row['pipeline_version'] == PIPELINE_VERSION
                usable = compatible and not missing_images
                document['status'] = ('ReadyWithWarnings' if document['warnings'] else 'Ready') if usable else 'Error'
                for image_path in dict.fromkeys(missing_images):
                    document['warnings'].append('Отсутствует сохранённое изображение: ' + image_path)
                if not compatible:
                    document['warnings'].append('Изменилась модель или обработка: загрузите файл повторно для переиндексации')
                result['documents'].append(document)
                if usable:
                    for chunk in chunks:
                        metadata = chunk['metadata']
                        metadata.update(documentId=str(row['id']), sourcePath=row['source_path'], text=chunk['text'], startOffset=chunk['start_offset'])
                        if metadata.get('imagePath'):
                            metadata['imagePath'] = str(self.data / 'images' / Path(metadata['imagePath']).name)
                        result['entries'].append({'chunk': metadata, 'embedding': chunk['embedding']})
            if params.get('remember', True):
                self.select_chat(params)
            return result

    def dispatch(self, method, params):
        if method not in ('bootstrap', 'create_chat', 'delete_chat', 'select_chat', 'question_history', 'save_question', 'save_context', 'finish_question', 'save_document', 'load_chat', 'bind_attachment'):
            raise ValueError("Неизвестная операция хранилища")
        return getattr(self, method)(params)

    def close(self):
        self.connection.close()
