"""Literal comparison of stored extraction results; no model or original-file reads."""
from collections import Counter
from difflib import SequenceMatcher
from pathlib import Path
import re

MAX_UNITS = 60000
MAX_CHARS = 16 * 1024 * 1024
MAX_CHANGES = 2000
MAX_REPORT_CHARS = 6 * 1024 * 1024


def normalized(text):
    # Preserve case, digits, signs and cell boundaries. Ignore layout whitespace only.
    return ' '.join(text.split())


def table_text(cells, header):
    width = max(len(cells), len(header))
    def line(values):
        escaped = [str(v).replace('|', r'\|').replace('\n', ' ') for v in values]
        return '| ' + ' | '.join(escaped + [''] * (width - len(escaped))) + ' |'
    return line(header) + '\n' + line(['---'] * width) + '\n' + line(cells)


def units(document):
    result, size, table_count = [], 0, 0
    section = ''
    blocks = document.get('blocks') or [dict(type='text', text=document['text'], pageNumber=0)]
    for index, block in enumerate(blocks):
        kind = block['type']
        page = block.get('pageNumber', 0)
        origin = dict(documentId=document['id'], sourcePath=document['sourcePath'],
                      pageFirst=page, pageLast=page, blockFirst=index, blockLast=index + 1,
                      tableIndices=[], section=block.get('section') or section, imagePath='')
        entries = []
        if kind == 'table':
            table_index = block.get('tableIndex', table_count)
            if table_index < 0:
                table_index = table_count
            table_count += 1
            rows = block.get('tableRows', [])
            origin['tableIndices'] = [table_index]
            for row, cells in enumerate(rows):
                # Empty cells/rows and duplicate rows are significant in technical tables.
                ref = dict(origin, section=f"{origin['section']} · Строка {row + 1}".strip(' ·'))
                ref['displayText'] = ' · '.join(cells)
                entries.append((('table', tuple(normalized(cell) for cell in cells)),
                                table_text(cells, rows[0]), ref))
        elif kind == 'image':
            image = block.get('imagePath', '')
            origin['imagePath'] = image
            text = block.get('text', '')
            label = ('[Текст изображения распознан OCR; возможны ошибки]\n'
                     if block.get('recognitionMethod') == 'windows-ocr'
                     else '[Описание изображения моделью; возможны ошибки]\n')
            entries.append((('image', Path(image).name, normalized(text)),
                            label + text, origin))
        else:
            for line in block.get('text', '').splitlines():
                if not line.strip():
                    continue
                if len(line) <= 180 and re.match(r'^(?:\d+(?:\.\d+)*[.)]?\s+|таблица\s+\d|раздел\s+\d)', line, re.I):
                    section = line.strip()
                ref = dict(origin, section=block.get('section') or section)
                entries.append((('text', normalized(line)), line, ref))
        for key, text, ref in entries:
            size += len(text.encode('utf-8'))
            if len(result) >= MAX_UNITS or size > MAX_CHARS:
                raise ValueError('Документ слишком велик для сравнения за один раз: предел 60 000 строк или 16 МиБ текста. Разделите его на части.')
            result.append(dict(key=key, text=text, reference=dict(ref, text=text)))
    return result


def compare_documents(left, right):
    before, after = units(left), units(right)
    keys_a, keys_b = [u['key'] for u in before], [u['key'] for u in after]
    # Trim equal edges even for heavily repeated documents. This also prevents a
    # one-line change in a huge repeated table from becoming a complete replacement.
    start = 0
    while start < min(len(before), len(after)) and keys_a[start] == keys_b[start]:
        start += 1
    end_a, end_b = len(before), len(after)
    while end_a > start and end_b > start and keys_a[end_a - 1] == keys_b[end_b - 1]:
        end_a -= 1
        end_b -= 1
    a, b = keys_a[start:end_a], keys_b[start:end_b]
    counts_a, counts_b = Counter(a), Counter(b)
    repeated = sum(count * counts_b[key] for key, count in counts_a.items()) > 2_000_000
    matcher = SequenceMatcher(None, a, b, autojunk=repeated)
    changes, counts, report_size = [], dict(added=0, removed=0, changed=0), 0
    limited = False

    def add(kind, old=None, new=None):
        nonlocal report_size, limited
        counts[kind] += 1
        size = sum(len(value['text'].encode('utf-8')) for value in (old, new) if value)
        if limited or len(changes) >= MAX_CHANGES or report_size + size > MAX_REPORT_CHARS:
            limited = True
            return
        report_size += size
        changes.append(dict(kind=kind, before=old['reference'] if old else None,
                            after=new['reference'] if new else None))

    def add_group(old, new):
        if len(old) == len(new) and all(x['key'][0] == y['key'][0] for x, y in zip(old, new)):
            for x, y in zip(old, new):
                if x['key'] != y['key']:
                    add('changed', x, y)
        else:
            for value in old:
                add('removed', old=value)
            for value in new:
                add('added', new=value)

    for tag, i, j, k, m in matcher.get_opcodes():
        if tag == 'equal':
            continue
        old, new = before[start + i:start + j], after[start + k:start + m]
        if tag == 'replace' and all(u['key'][0] == 'table' for u in old + new):
            # Match uniquely named rows when their values change alongside inserted
            # rows. Repeated/blank labels remain exact keys, never arbitrary anchors.
            def label(unit):
                return unit['key'][1][0] if unit['key'][1] else ''
            labels_a, labels_b = Counter(map(label, old)), Counter(map(label, new))
            unique = {value for value, count in labels_a.items() if value and count == labels_b[value] == 1}
            def anchor(unit):
                return ('label', label(unit)) if label(unit) in unique else unit['key']
            rows = SequenceMatcher(None, list(map(anchor, old)), list(map(anchor, new)), autojunk=True)
            for _, x, y, z, w in rows.get_opcodes():
                add_group(old[x:y], new[z:w])
        else:
            add_group(old, new)
    warnings = []
    for doc in (left, right):
        label = Path(doc['sourcePath']).name
        if doc.get('pendingImages', 0):
            warnings.append(f"{label} документ: {doc['pendingImages']} изображений ещё не распознано; сравнение неполное.")
        warnings.extend(f'{label} документ: {warning}' for warning in doc.get('warnings', []))
    if any(u['key'][0] == 'image' for u in before + after):
        warnings.append('Изображения сравниваются по сохранённым файлам и описаниям модели. Изменение описания не доказывает изменение фактов на картинке.')
    if repeated:
        warnings.append('Много повторяющихся строк: некоторые изменения могут быть объединены с соседними строками. Проверьте оба фрагмента.')
    if limited:
        warnings.append(f'Показаны первые {len(changes)} изменений из {sum(counts.values())}; достигнут предел размера отчёта. Для остальных сравните документы по частям.')
    return dict(left={k: left[k] for k in ('id', 'sourcePath')}, right={k: right[k] for k in ('id', 'sourcePath')},
                changes=changes, counts=counts, warnings=warnings, truncated=limited,
                partial=bool(left.get('pendingImages') or right.get('pendingImages') or left.get('warnings') or right.get('warnings')),
                unitsBefore=len(before), unitsAfter=len(after))
