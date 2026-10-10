"""Explicit, immutable snapshots. Chat copies never change another chat's version."""
import hashlib
import json
from pathlib import Path
import uuid


def _ready(repo, row):
    from repository import PIPELINE_VERSION
    if row['pending_images'] or row['pipeline_version'] != PIPELINE_VERSION:
        raise ValueError('Сначала завершите обработку документа с текущими настройками')
    count = repo.connection.execute(
        'SELECT count(*) AS total, count(*) FILTER (WHERE profile_id=%s AND cardinality(embedding)=%s) AS compatible '
        'FROM chunks WHERE document_id=%s', (repo.profile, repo.dimension, row['id'])).fetchone()
    if not count['total'] or count['compatible'] != count['total']:
        raise ValueError('Индекс несовместим с текущей моделью embeddings: переиндексируйте документ')
    for image in repo.connection.execute(
            "SELECT payload->>'imagePath' AS path FROM document_blocks WHERE document_id=%s AND kind='image'",
            (row['id'],)):
        if not image['path'] or not (repo.data / 'images' / Path(image['path']).name).is_file():
            raise ValueError('Отсутствует сохранённое изображение: переиндексируйте документ')


def _fingerprint(repo, row):
    digest = hashlib.sha256()
    def add(value):
        digest.update(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8'))
        digest.update(b'\n')
    add([row['content_sha256'], row['pipeline_version'], repo.profile, row['extracted_text'], row['warnings']])
    for block in repo.connection.execute('SELECT payload FROM document_blocks WHERE document_id=%s ORDER BY position', (row['id'],)):
        add(block['payload'])
    for chunk in repo.connection.execute('SELECT * FROM chunks WHERE document_id=%s ORDER BY position', (row['id'],)):
        metadata = {k: v for k, v in chunk['metadata'].items() if k not in ('documentId', 'sourcePath')}
        add([chunk['text'], chunk['start_offset'], metadata, chunk['profile_id'], chunk['embedding']])
    return digest.hexdigest()


def _copy(repo, source, target, chat_id):
    # All data and vectors stay in PostgreSQL. No OCR/model call or source-file access.
    repo.connection.execute(
        'INSERT INTO documents(id,chat_id,source_path,source_key,content_sha256,extracted_text,warnings,pipeline_version,pending_images) '
        'SELECT %s,%s,source_path,source_key,content_sha256,extracted_text,warnings,pipeline_version,0 FROM documents WHERE id=%s',
        (target, chat_id, source))
    repo.connection.execute('INSERT INTO document_blocks SELECT %s,position,kind,payload FROM document_blocks WHERE document_id=%s', (target, source))
    repo.connection.execute(
        "INSERT INTO chunks SELECT %s,position,text,start_offset,jsonb_set(metadata,'{documentId}',to_jsonb(%s::text)),profile_id,embedding "
        'FROM chunks WHERE document_id=%s', (target, str(target), source))


def publish(repo, params):
    with repo.connection.transaction():
        row = repo.connection.execute('SELECT * FROM documents WHERE id=%s AND chat_id=%s FOR SHARE',
                                      (params['documentId'], params['chatId'])).fetchone()
        if not row:
            raise ValueError('Документ не найден в указанном чате')
        _ready(repo, row)
        fingerprint = _fingerprint(repo, row)
        # Serialize the deduplication even if another worker publishes the same content.
        repo.connection.execute('SELECT pg_advisory_xact_lock(hashtextextended(%s,0))', (fingerprint,))
        existing = repo.connection.execute('SELECT id FROM document_library WHERE fingerprint=%s', (fingerprint,)).fetchone()
        if existing:
            return dict(id=str(existing['id']), existing=True)
        target = str(uuid.uuid4())
        _copy(repo, row['id'], target, None)
        repo.connection.execute('INSERT INTO document_library(id,fingerprint,profile_id) VALUES(%s,%s,%s)', (target, fingerprint, repo.profile))
        return dict(id=target, existing=False)


def list_documents(repo, params):
    from repository import PIPELINE_VERSION
    search = str(params.get('search', '')).strip()[:300]
    # Literal substring search: %, _ and quotes are data, not wildcard expressions.
    rows = repo.connection.execute(
        'SELECT d.id,d.source_path,d.content_sha256,d.pipeline_version,l.profile_id,l.created_at, '
        'length(d.extracted_text) AS characters, '
        '(SELECT count(*) FROM document_blocks b WHERE b.document_id=d.id) AS blocks '
        'FROM document_library l JOIN documents d ON d.id=l.id '
        'WHERE strpos(lower(d.source_path),lower(%s))>0 ORDER BY l.created_at DESC,l.id LIMIT 201', (search,)).fetchall()
    return dict(items=[dict(id=str(r['id']), sourcePath=r['source_path'],
                           sourceHash=r['content_sha256'], characters=r['characters'], blocks=r['blocks'],
                           createdAt=r['created_at'].isoformat(),
                           compatible=r['profile_id'] == repo.profile and r['pipeline_version'] == PIPELINE_VERSION)
                       for r in rows[:200]], hasMore=len(rows) > 200)


def attach(repo, params):
    ids = params['libraryIds']
    if not isinstance(ids, list) or not 1 <= len(ids) <= 20 or any(not isinstance(i, str) for i in ids) or len(set(ids)) != len(ids):
        raise ValueError('Выберите от одного до двадцати разных документов')
    chat = params['chatId']
    result = []
    with repo.connection.transaction():
        if not repo.connection.execute('SELECT id FROM chats WHERE id=%s FOR UPDATE', (chat,)).fetchone():
            raise ValueError('Чат не найден')
        if repo.connection.execute("SELECT 1 FROM messages WHERE chat_id=%s AND status='pending'", (chat,)).fetchone():
            raise ValueError('Дождитесь завершения ответа в этом чате')
        for library_id in ids:
            row = repo.connection.execute('SELECT d.*,l.fingerprint,l.profile_id FROM document_library l JOIN documents d ON d.id=l.id WHERE l.id=%s FOR SHARE OF d,l', (library_id,)).fetchone()
            if not row:
                raise ValueError('Документ уже удалён из библиотеки: обновите список')
            _ready(repo, row)
            if row['profile_id'] != repo.profile:
                raise ValueError('Документ из библиотеки использует другую модель embeddings')
            if repo.connection.execute("SELECT 1 FROM document_imports WHERE chat_id=%s AND source_key=%s AND status IN ('queued','running','paused','failed')", (chat, row['source_key'])).fetchone():
                raise ValueError('Этот файл уже находится в очереди обработки чата')
            existing = repo.connection.execute('SELECT * FROM documents WHERE chat_id=%s AND source_key=%s FOR UPDATE', (chat, row['source_key'])).fetchone()
            if existing:
                if existing['pending_images'] or _fingerprint(repo, existing) != row['fingerprint']:
                    raise ValueError('В чате уже есть другая версия этого файла. Добавьте выбранную версию в новый чат')
                result.append(dict(id=str(existing['id']), existing=True))
                continue
            target = str(uuid.uuid4())
            _copy(repo, library_id, target, chat)
            result.append(dict(id=target, existing=False))
    return dict(documents=result)


def delete(repo, params):
    with repo.connection.transaction():
        row = repo.connection.execute('SELECT d.id FROM documents d JOIN document_library l ON l.id=d.id WHERE l.id=%s AND d.chat_id IS NULL FOR UPDATE OF d', (params['id'],)).fetchone()
        if not row:
            raise ValueError('Документ уже удалён из библиотеки')
        images = {r['path'] for r in repo.connection.execute("SELECT payload->>'imagePath' AS path FROM document_blocks WHERE document_id=%s AND kind='image'", (row['id'],))}
        repo.connection.execute('DELETE FROM documents WHERE id=%s', (row['id'],))
    return dict(warnings=repo.cleanup_images(images))


ACTIONS = {'publish_library_document': publish, 'list_library': list_documents,
           'attach_library_documents': attach, 'delete_library_document': delete}
