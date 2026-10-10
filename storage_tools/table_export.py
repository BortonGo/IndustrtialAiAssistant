"""Small offline exporters. All XLSX values are strings, never executable formulas."""
import csv
import io
import re
from html import escape
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

NS = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'
REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
PACKAGE_REL = 'http://schemas.openxmlformats.org/package/2006/relationships'


def csv_bytes(rows):
    stream = io.StringIO(newline='')
    writer = csv.writer(stream, delimiter=';', quoting=csv.QUOTE_ALL, lineterminator='\r\n')
    protected = 0
    for row in rows:
        safe = []
        for value in row:
            if value.lstrip().startswith(('=', '+', '-', '@')) or value.startswith(('\t', '\r', '\n')):
                value = "'" + value; protected += 1
            safe.append(value)
        writer.writerow(safe)
    return stream.getvalue().encode('utf-8-sig'), protected


def column_name(index):
    name = ''
    while index:
        index, remainder = divmod(index - 1, 26)
        name = chr(65 + remainder) + name
    return name


def xml_text(value):
    if len(value.encode('utf-16-le')) // 2 > 32767:
        raise ValueError('В ячейке больше 32 767 символов — Excel не сохранит её полностью. Используйте CSV.')
    if re.search(r'[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]', value):
        raise ValueError('В ячейке есть управляющие символы, недопустимые в Excel. Используйте CSV.')
    # Preserve literal SpreadsheetML escape sequences and carriage returns.
    value = re.sub(r'_x[0-9a-fA-F]{4}_', lambda m: '_x005F_' + m[0][1:], value)
    return escape(value, quote=False).replace('\r', '&#13;')


def sheet_xml(rows, strings):
    width = max(map(len, rows), default=1)
    columns = []
    for column in range(width):
        lengths = [max(map(len, row[column].splitlines()), default=0) for row in rows[:200] if column < len(row)]
        size = max(12, min(50, max(lengths, default=12) + 3))
        columns.append(f'<col min="{column + 1}" max="{column + 1}" width="{size}" customWidth="1"/>')
    body = []
    for number, row in enumerate(rows, 1):
        cells = [f'<c r="{column_name(column)}{number}" t="s" s="{2 if number == 1 else 1}"><v>{strings[value]}</v></c>'
                 for column, value in enumerate(row, 1)]
        body.append(f'<row r="{number}">' + ''.join(cells) + '</row>')
    return (f'<worksheet xmlns="{NS}"><sheetViews><sheetView workbookViewId="0">'
            '<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
            '</sheetView></sheetViews><cols>' + ''.join(columns) + '</cols><sheetData>' + ''.join(body) + '</sheetData></worksheet>')


def xlsx_bytes(rows, metadata):
    # A deliberately limited SpreadsheetML writer: two sheets of literal text.
    # No formulas, hyperlinks, macros, external relationships or type inference.
    stream = io.BytesIO()
    strings = {value: index for index, value in enumerate(dict.fromkeys(value for row in rows + metadata for value in row))}
    with ZipFile(stream, 'w', ZIP_DEFLATED) as archive:
        def put(name, xml): archive.writestr(name, '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' + xml)
        content_types = [('sheet.main', 'workbook.xml'), ('styles', 'styles.xml'), ('sharedStrings', 'sharedStrings.xml')]
        overrides = ''.join(f'<Override PartName="/xl/{path}" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.{kind}+xml"/>' for kind, path in content_types)
        overrides += ''.join(f'<Override PartName="/xl/worksheets/sheet{i}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>' for i in (1, 2))
        put('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>' + overrides + '</Types>')
        put('_rels/.rels', f'<Relationships xmlns="{PACKAGE_REL}"><Relationship Id="rId1" Type="{REL}/officeDocument" Target="xl/workbook.xml"/></Relationships>')
        put('xl/workbook.xml', f'<workbook xmlns="{NS}" xmlns:r="{REL}"><sheets><sheet name="Таблица" sheetId="1" r:id="rId1"/><sheet name="Источник" sheetId="2" r:id="rId2"/></sheets></workbook>')
        put('xl/_rels/workbook.xml.rels', f'<Relationships xmlns="{PACKAGE_REL}">' + ''.join(f'<Relationship Id="rId{i}" Type="{REL}/worksheet" Target="worksheets/sheet{i}.xml"/>' for i in (1, 2)) + f'<Relationship Id="rId3" Type="{REL}/styles" Target="styles.xml"/><Relationship Id="rId4" Type="{REL}/sharedStrings" Target="sharedStrings.xml"/></Relationships>')
        put('xl/sharedStrings.xml', f'<sst xmlns="{NS}" uniqueCount="{len(strings)}">' + ''.join(f'<si><t xml:space="preserve">{xml_text(value)}</t></si>' for value in strings) + '</sst>')
        put('xl/styles.xml', f'<styleSheet xmlns="{NS}"><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><color rgb="FFFFFFFF"/><sz val="11"/><name val="Calibri"/></font></fonts>'
            '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FF34495E"/><bgColor indexed="64"/></patternFill></fill></fills>'
            '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
            '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
            '<cellXfs count="3"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
            '<xf numFmtId="49" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf>'
            '<xf numFmtId="49" fontId="1" fillId="2" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf></cellXfs>'
            '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>')
        put('xl/worksheets/sheet1.xml', sheet_xml(rows, strings))
        put('xl/worksheets/sheet2.xml', sheet_xml(metadata, strings))
    return stream.getvalue()


def export_table(report, table_id, format_):
    table = next((t for t in report['tables'] if t['id'] == table_id), None)
    if table is None: raise ValueError('Таблица не найдена. Обновите список.')
    if format_ not in ('csv', 'xlsx'): raise ValueError('Поддерживаются CSV и XLSX')
    ref = table['reference']
    metadata = [['Свойство', 'Значение'], ['Документ', report['sourcePath']], ['ID документа', report['documentId']],
                ['Таблица в списке', str(table['number'])], ['Страница', str(ref['pageFirst']) if ref['pageFirst'] else 'Не определена'],
                ['Раздел', ref['section']], ['Происхождение', {'native': 'Ячейки исходной таблицы', 'recognition': 'Распознавание изображения моделью', 'text_markup': 'Таблица в текстовой разметке'}[table['origin']]],
                ['Блок документа', str(ref['blockFirst'] + 1)], ['Таблицы по порядку в документе', ', '.join(str(i + 1) for i in ref['tableIndices'])],
                ['Изображение', ref['imagePath']],
                ['Полнота документа', 'Неполная' if report['partial'] else 'Обработка завершена'],
                ['Снимок', report['snapshot']], ['Формат ячеек', 'Текст. Числа, коды и единицы не преобразованы.']]
    metadata.extend(['Предупреждение', warning] for warning in report['warnings'])
    if format_ == 'csv': data, protected = csv_bytes(table['rows'])
    else: data, protected = xlsx_bytes(table['rows'], metadata), 0
    filename = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', Path(report['sourcePath']).stem)[:100]
    return data, f'{filename}-table-{table["number"]}.{format_}', protected
