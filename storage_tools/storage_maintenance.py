"""Storage accounting and conservative collection of unreferenced managed images.

All operations run in the single storage worker. Never delete source files, reader
caches or PostgreSQL files; paused imports and historical citations are owners too.
"""
import ntpath
import re
import shutil
import stat
import uuid


MANAGED_FILE = re.compile(r'[0-9a-f]{64}\.(png|tmp)')


def _linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 0x400)


def _assets(repo):
    root = repo.data / 'images'
    if root.exists():
        if _linked(root.lstat()) or root.resolve() != repo.data.resolve() / 'images':
            raise ValueError('Папка изображений является ссылкой; очистка отключена')
    return root


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def referenced_images(repo):
    rows = repo.connection.execute("""
        SELECT payload->>'imagePath' AS path FROM document_blocks
        UNION SELECT metadata->>'imagePath' FROM chunks
        UNION SELECT preview_image FROM message_attachments
        UNION SELECT source_path FROM documents
        UNION SELECT source_path FROM message_attachments
        UNION SELECT source_path FROM document_imports
        UNION SELECT r->>'imagePath' FROM messages m, jsonb_array_elements(m.source_references) r
        UNION SELECT b->>'imagePath' FROM document_imports i,
            jsonb_array_elements(CASE WHEN jsonb_typeof(i.checkpoint->'document'->'blocks')='array'
                THEN i.checkpoint->'document'->'blocks' ELSE '[]'::jsonb END) b
            WHERE i.status NOT IN ('complete','cancelled')
        UNION SELECT c.payload->'chunk'->>'imagePath' FROM document_import_chunks c
            JOIN document_imports i ON i.id=c.import_id WHERE i.status NOT IN ('complete','cancelled')
    """)
    # Checkpoints use absolute paths, persisted documents use managed basenames.
    return {ntpath.basename(row['path']) for row in rows if row['path']}


def cleanup_images(repo, names, expected=None):
    """Return actual deletions; expected pins cleanup to the previewed file versions."""
    warnings, deleted, freed = [], 0, 0
    root = _assets(repo)
    referenced = referenced_images(repo)
    for name in names:
        if not isinstance(name, str) or not MANAGED_FILE.fullmatch(name):
            warnings.append('Пропущено изображение с некорректным именем')
            continue
        if name in referenced:
            continue
        target = root / name
        try:
            info = target.lstat()
            if _linked(info) or not stat.S_ISREG(info.st_mode):
                warnings.append(f'Пропущена ссылка или нестандартный файл: {name}')
                continue
            if expected is not None and _identity(info) != expected[name]:
                warnings.append(f'Файл изменился после расчёта: {name}')
                continue
            target.unlink()
            deleted += 1
            freed += info.st_size
        except FileNotFoundError:
            pass
        except OSError as error:
            warnings.append(f'Не удалось удалить сохранённую копию {name}: {error}')
    return dict(deletedFiles=deleted, freedBytes=freed, warnings=warnings)


def _folder_size(root, warnings):
    """Read-only, without following symlinks or Windows junctions."""
    total, pending = 0, [root]
    while pending:
        path = pending.pop()
        try:
            info = path.lstat()
            if _linked(info):
                warnings.append(f'Размер ссылки не учитывается: {path.name}')
            elif stat.S_ISDIR(info.st_mode):
                pending.extend(path.iterdir())
            elif stat.S_ISREG(info.st_mode):
                total += info.st_size
        except OSError:
            warnings.append(f'Не удалось измерить файл: {path.name}')
    return total


def _busy_reason(repo):
    if repo.connection.execute("SELECT 1 FROM messages WHERE status='pending' LIMIT 1").fetchone():
        return 'Очистка доступна после завершения ответа.'
    if repo.connection.execute("SELECT 1 FROM document_imports WHERE status IN ('queued','running') LIMIT 1").fetchone():
        return 'Для очистки приостановите очередь обработки документов.'
    return ''


def storage_usage(repo, params):
    warnings, candidates = [], {}
    root = _assets(repo)
    referenced = referenced_images(repo)
    image_bytes, image_count, reclaimable = 0, 0, 0
    if root.exists():
        for path in root.iterdir():
            try:
                info = path.lstat()
                if _linked(info) or not stat.S_ISREG(info.st_mode):
                    warnings.append(f'В изображениях пропущена ссылка или папка: {path.name}')
                    continue
                image_bytes += info.st_size
                image_count += 1
                if MANAGED_FILE.fullmatch(path.name) and path.name not in referenced:
                    candidates[path.name] = _identity(info)
                    reclaimable += info.st_size
            except OSError:
                warnings.append(f'Не удалось измерить изображение: {path.name}')
    cluster = repo.data / 'postgres'
    cluster_bytes = _folder_size(cluster, warnings) if cluster.exists() else 0
    other_bytes = 0
    for path in repo.data.iterdir():
        if path.name not in ('postgres', 'images'):
            other_bytes += _folder_size(path, warnings)
    counts = repo.connection.execute("""SELECT
        (SELECT count(*) FROM chats) AS chats,
        (SELECT count(*) FROM documents WHERE chat_id IS NOT NULL) AS documents,
        (SELECT count(*) FROM document_library) AS library,
        (SELECT count(*) FROM chunks) AS chunks,
        (SELECT count(*) FROM document_imports WHERE status NOT IN ('complete','cancelled')) AS imports
    """).fetchone()
    db_bytes = repo.connection.execute('SELECT pg_database_size(current_database()) AS size').fetchone()['size']
    token = uuid.uuid4().hex
    # Only the latest preview is valid; another window or refresh invalidates it.
    repo._storage_cleanup_plan = (token, candidates)
    return dict(directory=str(repo.data.resolve()), databaseBytes=db_bytes, clusterBytes=cluster_bytes,
                imageBytes=image_bytes, imageFiles=image_count, otherBytes=other_bytes,
                totalBytes=cluster_bytes + image_bytes + other_bytes,
                freeBytes=shutil.disk_usage(repo.data).free, counts=counts,
                reclaimableBytes=reclaimable, reclaimableFiles=len(candidates), cleanupToken=token,
                blockedReason=_busy_reason(repo), warnings=warnings[:20], approximate=bool(warnings))


def cleanup_storage(repo, params):
    plan = getattr(repo, '_storage_cleanup_plan', None)
    if not plan or params.get('token') != plan[0]:
        raise ValueError('Расчёт места устарел. Нажмите «Обновить» и повторите очистку.')
    repo._storage_cleanup_plan = None
    reason = _busy_reason(repo)
    if reason:
        raise ValueError(reason)
    # Re-read every owner after confirmation. New files are never included.
    return cleanup_images(repo, plan[1], expected=plan[1])


ACTIONS = {'storage_usage': storage_usage, 'cleanup_storage': cleanup_storage}
