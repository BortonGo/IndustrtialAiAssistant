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
from document_comparison import compare_documents

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
            self.connection.execute("UPDATE document_imports SET status='paused', progress='Импорт прерван при закрытии; можно продолжить', updated_at=now() WHERE status='running'")

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
                    message['source_references'] = self.references_json(message['source_references'])
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
            if self.connection.execute("SELECT 1 FROM document_imports WHERE chat_id=%s AND status='running' LIMIT 1", (chat_id,)).fetchone():
                raise ValueError("Сначала приостановите импорт документов этого чата")
            images = {row['path'] for row in self.connection.execute(
                "SELECT b.payload->>'imagePath' AS path FROM document_blocks b "
                "JOIN documents d ON d.id=b.document_id WHERE d.chat_id=%s AND b.kind='image'", (chat_id,))}
            images.update(row['preview_image'] for row in self.connection.execute(
                "SELECT a.preview_image FROM message_attachments a JOIN messages m ON m.id=a.message_id WHERE m.chat_id=%s AND a.preview_image<>''", (chat_id,)))
            images.update(row['path'] for row in self.connection.execute(
                "SELECT r->>'imagePath' AS path FROM messages m, jsonb_array_elements(m.source_references) r "
                "WHERE m.chat_id=%s AND r->>'imagePath'<>''", (chat_id,)))
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
                        "UNION ALL SELECT 1 FROM message_attachments WHERE preview_image=%s "
                        "UNION ALL SELECT 1 FROM messages m, jsonb_array_elements(m.source_references) r "
                        "WHERE r->>'imagePath'=%s LIMIT 1", (name, name, name, name)).fetchone():
                    continue
                target = assets / name
                if assets.is_symlink() or assets.resolve() != self.data.resolve() / 'images' or target.is_symlink():
                    raise OSError('Путь изображения выходит за папку хранения')
                target.unlink(missing_ok=True)
            except (OSError, psycopg.Error) as error:
                warnings.append(f'Не удалось удалить сохранённую копию {name}: {error}')
        return {'deletedChatId': chat_id, 'currentChatId': selected, 'replacementChat': replacement, 'warnings': warnings}

    def references_json(self, references):
        return [dict(ref, imagePath=str(self.data / 'images' / Path(ref['imagePath']).name)
                     if ref.get('imagePath') else '') for ref in references]

    def save_context(self, params):
        references = params.get('references', [])
        if not isinstance(references, list) or len(references) > 64:
            raise ValueError('Некорректный список источников')
        stored = []
        with self.connection.transaction():
            question = self.connection.execute("SELECT chat_id FROM messages WHERE id=%s AND role='user' AND status='pending' FOR UPDATE", (params['id'],)).fetchone()
            if not question:
                raise ValueError("Вопрос для сохранения источников не найден")
            for number, ref in enumerate(references, 1):
                if not isinstance(ref, dict) or not isinstance(ref.get('text'), str) or not ref['text'].strip():
                    raise ValueError('Пустой фрагмент источника')
                document = self.connection.execute('SELECT source_path FROM documents WHERE id=%s AND chat_id=%s',
                                                   (ref.get('documentId'), question['chat_id'])).fetchone()
                if not document:
                    raise ValueError('Источник не принадлежит чату вопроса')
                item = dict(number=number, documentId=ref['documentId'], sourcePath=document['source_path'], text=ref['text'])
                for key, default in (('pageFirst', 0), ('pageLast', 0), ('blockFirst', -1), ('blockLast', -1)):
                    value = ref.get(key, default)
                    if type(value) is not int or value < default:
                        raise ValueError('Некорректное положение источника')
                    item[key] = value
                if item['pageLast'] < item['pageFirst'] or item['blockLast'] < item['blockFirst']:
                    raise ValueError('Некорректный диапазон источника')
                tables = ref.get('tableIndices', [])
                if not isinstance(tables, list) or any(type(n) is not int or n < 0 for n in tables):
                    raise ValueError('Некорректные номера таблиц')
                item['tableIndices'] = tables
                section = ref.get('section', '')
                if not isinstance(section, str):
                    raise ValueError('Некорректный раздел источника')
                item['section'] = section
                image = ref.get('imagePath', '')
                item['imagePath'] = ''
                if image:
                    if not isinstance(image, str):
                        raise ValueError('Некорректное изображение источника')
                    # A just-imported attachment can still reference the extraction cache.
                    # Resolve by content to its existing durable copy in this document.
                    if not Path(image).is_file() or Path(image).stat().st_size > 16 * 1024 * 1024:
                        raise ValueError('Изображение источника недоступно или слишком велико')
                    name = sha256(image) + '.png'
                    asset = self.data / 'images' / name
                    if (not asset.is_file()
                            or not self.connection.execute("SELECT 1 FROM document_blocks WHERE document_id=%s AND payload->>'imagePath'=%s",
                                                           (ref['documentId'], name)).fetchone()):
                        raise ValueError('Изображение не принадлежит источнику')
                    item['imagePath'] = name
                stored.append(item)
            self.connection.execute("UPDATE messages SET context_snapshot=%s,source_references=%s WHERE id=%s",
                                    (params['context'], Jsonb(stored), params['id']))
        return {'references': self.references_json(stored)}

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
        pending = document.get('pendingImages', 0)
        if type(pending) is not int or pending < 0 or (pending and not params.get('importId')):
            raise ValueError('Некорректный предварительный документ')
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
            if params.get('importId'):
                job = self.connection.execute('SELECT * FROM document_imports WHERE id=%s FOR UPDATE', (params['importId'],)).fetchone()
                if (not job or job['status'] != 'running' or str(job['chat_id']) != params['chatId'] or
                        str(job['document_id']) != document['id'] or job['source_key'] != source_key or
                        job['source_sha256'] != source_hash or job['profile_id'] != self.profile):
                    raise ValueError('Задание импорта изменилось или исходный файл был изменён; добавьте файл заново')
            owner = self.connection.execute('SELECT chat_id FROM documents WHERE id=%s FOR UPDATE', (document['id'],)).fetchone()
            if owner and str(owner['chat_id']) != params['chatId']:
                raise ValueError('Документ принадлежит другому чату')
            previous = self.connection.execute("SELECT id FROM documents WHERE chat_id=%s AND source_key=%s", (params['chatId'], source_key)).fetchone()
            if pending and previous:
                # A completed version (or a foreground attachment) must never regress to a preview.
                self.connection.execute('UPDATE document_imports SET document_id=%s WHERE id=%s', (previous['id'], params['importId']))
                self.advance_import({'id': params['importId']})
                return {'published': False, 'documentId': str(previous['id'])}
            if params.get('importId') and previous and str(previous['id']) != document['id']:
                # A foreground attachment may have imported this file while the job was paused.
                # Keep its ID so existing message attachments retain their document reference.
                document['id'] = str(previous['id'])
                for entry in entries:
                    entry['chunk']['documentId'] = document['id']
                self.connection.execute('UPDATE document_imports SET document_id=%s WHERE id=%s', (document['id'], params['importId']))
            if previous and str(previous['id']) != document['id']:
                self.connection.execute("DELETE FROM documents WHERE id=%s", (previous['id'],))
            if previous and str(previous['id']) == document['id']:
                self.connection.execute('DELETE FROM document_blocks WHERE document_id=%s', (document['id'],))
                self.connection.execute('DELETE FROM chunks WHERE document_id=%s', (document['id'],))
            self.connection.execute("INSERT INTO documents(id,chat_id,source_path,source_key,content_sha256,extracted_text,warnings,pipeline_version,pending_images) VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s) "
                                    "ON CONFLICT(id) DO UPDATE SET source_path=excluded.source_path,source_key=excluded.source_key,content_sha256=excluded.content_sha256,extracted_text=excluded.extracted_text,warnings=excluded.warnings,pipeline_version=excluded.pipeline_version,pending_images=excluded.pending_images WHERE documents.chat_id=excluded.chat_id",
                                    (document['id'], params['chatId'], source, source_key, source_hash, document['text'], Jsonb(document['warnings']), PIPELINE_VERSION, pending))
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
            if params.get('importId'):
                if pending:
                    self.advance_import({'id': params['importId']})
                else:
                    self.connection.execute("UPDATE document_imports SET status='complete', progress='Готов к поиску', error='', checkpoint='{}', updated_at=now() WHERE id=%s", (params['importId'],))
                    self.connection.execute('DELETE FROM document_import_chunks WHERE import_id=%s', (params['importId'],))
        return {'published': True, 'documentId': document['id']}

    def advance_import(self, params):
        with self.connection.transaction():
            row = self.connection.execute("SELECT * FROM document_imports WHERE id=%s AND status='running' FOR UPDATE", (params['id'],)).fetchone()
            if not row or row['profile_id'] != self.profile or sha256(row['source_path']) != row['source_sha256']:
                raise ValueError('Задание или исходный файл изменились')
            self.connection.execute("UPDATE document_imports SET native_done=true,checkpoint='{}',updated_at=now() WHERE id=%s", (row['id'],))
            self.connection.execute('DELETE FROM document_import_chunks WHERE import_id=%s', (row['id'],))
        return {}

    @staticmethod
    def import_json(row, checkpoint=False):
        result = {'id': str(row['id']), 'chatId': str(row['chat_id']), 'documentId': str(row['document_id']),
                  'sourcePath': row['source_path'], 'status': row['status'], 'progress': row['progress'], 'error': row['error'], 'nativeDone': row['native_done']}
        if checkpoint:
            result['checkpoint'] = row['checkpoint']
        return result

    def enqueue_import(self, params):
        path = Path(params['sourcePath']).resolve(strict=True)
        if not path.is_file() or path.suffix.lower() not in ('.txt','.pdf','.doc','.docx','.png','.jpg','.jpeg','.webp','.bmp'):
            raise ValueError('Неподдерживаемый файл для импорта')
        if path.stat().st_size > int(params.get('maxFileSize', 100 * 1024 * 1024)):
            raise ValueError('Документ превышает лимит размера файла')
        key = os.path.normcase(str(path))
        with self.connection.transaction():
            existing = self.connection.execute("SELECT * FROM document_imports WHERE chat_id=%s AND source_key=%s AND status IN ('queued','running','paused','failed')", (params['chatId'], key)).fetchone()
            if existing:
                return {'job': self.import_json(existing)}
            document = self.connection.execute('SELECT id FROM documents WHERE chat_id=%s AND source_key=%s', (params['chatId'], key)).fetchone()
            row = self.connection.execute("INSERT INTO document_imports(id,chat_id,document_id,source_path,source_key,source_sha256,profile_id,status,progress) VALUES(%s,%s,%s,%s,%s,%s,%s,'queued','В очереди') RETURNING *",
                (params['id'], params['chatId'], document['id'] if document else uuid.uuid4(), str(path), key, sha256(path), self.profile)).fetchone()
            return {'job': self.import_json(row)}

    def list_imports(self, params):
        return {'jobs': [self.import_json(row) for row in self.connection.execute(
            "SELECT id,chat_id,document_id,source_path,status,progress,error,native_done FROM document_imports WHERE status NOT IN ('complete','cancelled') ORDER BY created_at,id")]}

    def start_import(self, params):
        with self.connection.transaction():
            row = self.connection.execute('SELECT * FROM document_imports WHERE id=%s FOR UPDATE', (params['id'],)).fetchone()
            if not row or row['status'] != 'queued':
                raise ValueError('Задание больше не ожидает обработки')
            try:
                if sha256(row['source_path']) != row['source_sha256']:
                    raise ValueError('Файл изменился. Удалите задание из очереди и добавьте файл заново')
            except (OSError, ValueError) as error:
                row = self.connection.execute("UPDATE document_imports SET status='failed',error=%s,updated_at=now() WHERE id=%s RETURNING *", (str(error), row['id'])).fetchone()
                return {'job': self.import_json(row)}
            if row['profile_id'] != self.profile:
                self.connection.execute('DELETE FROM document_import_chunks WHERE import_id=%s', (row['id'],))
            row = self.connection.execute("UPDATE document_imports SET status='running',error='',profile_id=%s,checkpoint=CASE WHEN profile_id=%s THEN checkpoint ELSE '{}'::jsonb END,updated_at=now() WHERE id=%s RETURNING *", (self.profile, self.profile, row['id'])).fetchone()
            if row['checkpoint']:
                row['checkpoint']['entries'] = [r['payload'] for r in self.connection.execute('SELECT payload FROM document_import_chunks WHERE import_id=%s ORDER BY position', (row['id'],))]
            # Reuse text embeddings even if image insertion changes offsets/block numbers.
            # Keep a previous published version available throughout reindexing.
            existing = self.connection.execute('SELECT id FROM documents WHERE chat_id=%s AND source_key=%s', (row['chat_id'], row['source_key'])).fetchone()
            reusable = []
            if existing:
                reusable = [{'text': r['text'], 'embedding': r['embedding']} for r in self.connection.execute(
                    'SELECT text,embedding FROM chunks WHERE document_id=%s AND profile_id=%s ORDER BY position', (existing['id'], self.profile))]
                if not row['checkpoint']:
                    row = self.connection.execute('UPDATE document_imports SET native_done=true WHERE id=%s RETURNING *', (row['id'],)).fetchone()
            return {'job': self.import_json(row, True), 'reusableEntries': reusable}

    def update_import(self, params):
        transitions = {'queued': {'paused','cancelled'}, 'paused': {'queued','cancelled'},
                       'failed': {'queued','cancelled'}, 'running': {'queued','paused','failed','cancelled','running'}}
        with self.connection.transaction():
            row = self.connection.execute('SELECT * FROM document_imports WHERE id=%s FOR UPDATE', (params['id'],)).fetchone()
            if not row or params['status'] not in transitions.get(row['status'], set()):
                raise ValueError('Состояние задания уже изменилось')
            row = self.connection.execute("UPDATE document_imports SET status=%s,progress=%s,error=%s,checkpoint=CASE WHEN %s='cancelled' THEN '{}'::jsonb ELSE checkpoint END,updated_at=now() WHERE id=%s RETURNING *",
                (params['status'], params.get('progress', row['progress']), params.get('error', ''), params['status'], row['id'])).fetchone()
            if params['status'] == 'cancelled':
                self.connection.execute('DELETE FROM document_import_chunks WHERE import_id=%s', (row['id'],))
            return {'job': self.import_json(row)}

    def checkpoint_import(self, params):
        with self.connection.transaction():
            row = self.connection.execute("SELECT * FROM document_imports WHERE id=%s AND status='running' FOR UPDATE", (params['id'],)).fetchone()
            if not row or row['profile_id'] != self.profile:
                raise ValueError('Задание не выполняется или модель изменилась')
            checkpoint = row['checkpoint']
            if 'document' in params:
                document = params['document']
                if (document['id'] != str(row['document_id']) or
                        os.path.normcase(os.path.abspath(document['sourcePath'])) != row['source_key']):
                    raise ValueError('Документ не принадлежит заданию')
                checkpoint = {'document': document}
                self.connection.execute('DELETE FROM document_import_chunks WHERE import_id=%s', (row['id'],))
            if 'entries' in params:
                count = self.connection.execute('SELECT count(*) AS count FROM document_import_chunks WHERE import_id=%s', (row['id'],)).fetchone()['count']
                if not checkpoint or count != params['offset']:
                    raise ValueError('Нарушен порядок сохранения индекса')
                for entry in params['entries']:
                    vector = entry['embedding']
                    if (entry['chunk']['documentId'] != str(row['document_id']) or len(vector) != self.dimension or
                            not all(isinstance(x, (float, int)) and not isinstance(x, bool) and math.isfinite(x) for x in vector) or not any(vector)):
                        raise ValueError('Некорректный фрагмент индекса')
                with self.connection.cursor() as cursor:
                    cursor.executemany('INSERT INTO document_import_chunks VALUES(%s,%s,%s)',
                        [(row['id'], count + i, Jsonb(entry)) for i, entry in enumerate(params['entries'])])
            if 'document' in params:
                self.connection.execute('UPDATE document_imports SET checkpoint=%s WHERE id=%s', (Jsonb(checkpoint), row['id']))
            self.connection.execute('UPDATE document_imports SET progress=%s,updated_at=now() WHERE id=%s', (params['progress'], row['id']))
        return {}

    def load_chat(self, params):
        with self.connection.transaction():
            if not self.connection.execute("SELECT 1 FROM chats WHERE id=%s", (params['chatId'],)).fetchone():
                raise ValueError("Чат не найден")
            result = {"documents": [], "entries": []}
            for row in self.connection.execute("SELECT * FROM documents WHERE chat_id=%s ORDER BY created_at,id", (params['chatId'],)):
                document = {"id": str(row['id']), "sourcePath": row['source_path'], "text": row['extracted_text'], "warnings": row['warnings'], "blocks": [], "pendingImages": row['pending_images']}
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
                if usable and row['pending_images']:
                    document['status'] = 'TextReady'
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

    def compare_documents(self, params):
        ids = params.get('documentIds')
        if not isinstance(ids, list) or len(ids) != 2 or ids[0] == ids[1]:
            raise ValueError('Выберите два разных документа одного чата')
        documents = []
        # One snapshot: an import cannot mix old text with newly replaced blocks.
        with self.connection.transaction():
            self.connection.execute('SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY')
            for document_id in ids:
                row = self.connection.execute('SELECT * FROM documents WHERE id=%s AND chat_id=%s',
                                              (document_id, params['chatId'])).fetchone()
                if not row:
                    raise ValueError('Документ не найден в выбранном чате')
                blocks = [r['payload'] for r in self.connection.execute('SELECT payload FROM document_blocks WHERE document_id=%s ORDER BY position', (document_id,))]
                warnings = list(row['warnings'])
                for block in blocks:
                    if block.get('imagePath'):
                        block['imagePath'] = str(self.data / 'images' / Path(block['imagePath']).name)
                        if not Path(block['imagePath']).is_file():
                            warnings.append('Сохранённое изображение недоступно: ' + block['imagePath'])
                documents.append(dict(id=str(row['id']), sourcePath=row['source_path'], text=row['extracted_text'],
                                      blocks=blocks, pendingImages=row['pending_images'], warnings=warnings))
        return compare_documents(*documents)

    def dispatch(self, method, params):
        if method not in ('bootstrap', 'create_chat', 'delete_chat', 'select_chat', 'question_history', 'save_question', 'save_context', 'finish_question', 'save_document', 'load_chat', 'bind_attachment',
                          'enqueue_import', 'list_imports', 'start_import', 'update_import', 'checkpoint_import', 'advance_import', 'compare_documents'):
            raise ValueError("Неизвестная операция хранилища")
        return getattr(self, method)(params)

    def close(self):
        self.connection.close()
