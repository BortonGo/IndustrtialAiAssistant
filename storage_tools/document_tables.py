"""Tables from stored blocks; conservative Markdown parsing, no new inference."""
import hashlib
import json
import re

MAX_TABLES = 500
MAX_CELLS = 200000
MAX_BYTES = 16 * 1024 * 1024
MAX_COLUMNS = 512


def split_row(line):
    line = line.strip()
    cells, cell, separators = [], [], 0
    index = 0
    while index < len(line):
        char = line[index]
        if char == '\\' and index + 1 < len(line) and line[index + 1] in ('|', '\\'):
            cell.append(line[index + 1]); index += 2; continue
        if char == '|':
            cells.append(''.join(cell).strip()); cell = []; separators += 1
        else:
            cell.append(char)
        index += 1
    cells.append(''.join(cell).strip())
    if not separators:
        return None
    if line.startswith('|'): cells.pop(0)
    # A trailing escaped pipe belongs to the last cell.
    trailing_slashes = len(line[:-1]) - len(line[:-1].rstrip('\\'))
    if line.endswith('|') and trailing_slashes % 2 == 0: cells.pop()
    return cells


def markdown_tables(text):
    lines, index, fence = text.splitlines(), 0, ''
    while index < len(lines):
        marker = re.match(r'^\s{0,3}(`{3,}|~{3,})', lines[index])
        if marker:
            token = marker[1]
            if not fence: fence = token
            elif token[0] == fence[0] and len(token) >= len(fence): fence = ''
            index += 1; continue
        if fence or index + 1 >= len(lines):
            index += 1; continue
        header, divider = split_row(lines[index]), split_row(lines[index + 1])
        if not header or not divider or not all(re.fullmatch(r':?-{3,}:?', cell) for cell in divider):
            index += 1; continue
        start, rows = index, [header]
        malformed = len(header) != len(divider)
        index += 2
        while index < len(lines) and lines[index].strip():
            row = split_row(lines[index])
            if row is None: break
            malformed |= len(row) != len(header)
            rows.append(row); index += 1
        yield start, rows, '\n'.join(lines[start:index]), malformed


def table_markdown(rows):
    def line(row):
        return '| ' + ' | '.join(cell.replace('\\', '\\\\').replace('|', r'\|').replace('\n', ' ') for cell in row) + ' |'
    if not rows: return ''
    return '\n'.join([line(rows[0]), line(['---'] * len(rows[0]))] + [line(row) for row in rows[1:]])


def collect_tables(document):
    tables, warnings = [], list(document.get('warnings', []))
    total_cells = total_bytes = skipped = 0
    pending = document.get('pendingImages', 0)
    if pending:
        warnings.append(f'Ещё не распознано изображений: {pending}. Список таблиц может быть неполным.')
    caption = ''
    blocks = document.get('blocks') or [dict(type='text', text=document.get('text', ''))]
    for position, block in enumerate(blocks):
        kind = block.get('type')
        if kind == 'table':
            candidates = [(0, block.get('tableRows', []), '', False)]
        else:
            candidates = markdown_tables(block.get('text', ''))
        for line, rows, raw, malformed in candidates:
            if malformed:
                skipped += 1
                warnings.append(f'Блок {position + 1}, строка {line + 1}: разное число столбцов в Markdown. Таблица пропущена; проверьте источник.')
                continue
            if not rows or not any(rows): continue
            columns = max(map(len, rows))
            cells = len(rows) * columns
            size = sum(len(cell.encode('utf-8')) for row in rows for cell in row)
            if len(tables) >= MAX_TABLES or columns > MAX_COLUMNS or total_cells + cells > MAX_CELLS or total_bytes + size > MAX_BYTES:
                skipped += 1; continue
            total_cells += cells; total_bytes += size
            number = len(tables) + 1
            origin = 'native' if kind == 'table' else 'recognition' if kind == 'image' else 'text_markup'
            table_index = block.get('tableIndex', -1)
            ref = dict(documentId=document['id'], sourcePath=document['sourcePath'],
                       pageFirst=block.get('pageNumber', 0), pageLast=block.get('pageNumber', 0),
                       blockFirst=position, blockLast=position + 1,
                       tableIndices=[table_index] if table_index >= 0 else [],
                       section=block.get('section') or caption, imagePath=block.get('imagePath', ''),
                       text=raw or table_markdown(rows))
            tables.append(dict(id=f'{position}:{line}', number=number, rows=rows, columns=columns,
                               origin=origin, reference=ref))
        if kind == 'text':
            lines = [line.strip() for line in block.get('text', '').splitlines() if line.strip()]
            if lines and len(lines[-1]) <= 180: caption = lines[-1]
    if skipped:
        warnings.append(f'Пропущено таблиц: {skipped}. Предел просмотра: {MAX_TABLES} таблиц, {MAX_CELLS} ячеек, {MAX_COLUMNS} столбцов, 16 МиБ текста. Неразобранные таблицы также учитываются.')
    if any(table['origin'] == 'recognition' for table in tables):
        warnings.append('Таблицы из изображений получены моделью: сверьте числа и столбцы с изображением перед использованием.')
    snapshot = hashlib.sha256(json.dumps(document, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest()
    return dict(documentId=document['id'], sourcePath=document['sourcePath'], tables=tables,
                warnings=warnings, partial=bool(pending or document.get('warnings') or skipped),
                skipped=skipped, snapshot=snapshot)
