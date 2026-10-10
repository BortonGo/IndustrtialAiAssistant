"""Stored tables and offline CSV/XLSX export; no models or user data."""
import copy
import csv
import io
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'storage_tools'))
from document_tables import collect_tables, markdown_tables, split_row
from table_export import csv_bytes, export_table, NS


def document(blocks, **kwargs):
    return dict(id='document-1', sourcePath='C:/docs/Протокол.docx', blocks=blocks, text='', warnings=[], pendingImages=0, **kwargs)


def native(rows):
    return dict(type='table', tableRows=rows, tableIndex=2, pageNumber=7)


class TableTests(unittest.TestCase):
    def test_native_order_empty_cells_repetitions_and_positions(self):
        rows = [['Код', 'Значение', 'Единицы'], ['00017', '7,5', 'bar'], ['', '', ''], ['00017', '7,5', 'bar'], []]
        report = collect_tables(document([dict(type='text', text='5.4 Протокол'), native(rows)]))
        table = report['tables'][0]
        self.assertEqual(table['rows'], rows)
        self.assertEqual(table['reference']['pageFirst'], 7)
        self.assertEqual(table['reference']['tableIndices'], [2])
        self.assertEqual(table['reference']['section'], '5.4 Протокол')
        self.assertEqual(table['origin'], 'native')
        self.assertFalse(report['partial'])

    def test_markdown_escaped_pipes_and_multiple_tables(self):
        text = '| Код | Значение |\n|:---|---:|\n| A\\|B | 01 |\n\nДругая таблица\nX | Y\n--- | ---\n02 | 3.5'
        result = list(markdown_tables(text))
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0][1][1], ['A|B', '01'])
        self.assertEqual(result[1][1][1], ['02', '3.5'])
        self.assertEqual(split_row('| one \\|'), ['one |'])
        self.assertEqual(split_row('| A |'), ['A'])

    def test_no_guessing_columns_or_fenced_code(self):
        text = '```\n| a | b |\n|---|---|\n|1|2|\n```\nПротокол  Значение\nILS  0x01'
        self.assertEqual(collect_tables(document([dict(type='text', text=text)]))['tables'], [])

    def test_malformed_markdown_is_warned_not_silently_truncated(self):
        report = collect_tables(document([dict(type='text', text='| A | B |\n|---|---|\n|1|2|3|')]))
        self.assertEqual(report['tables'], [])
        self.assertTrue(report['partial'])
        self.assertEqual(report['skipped'], 1)
        self.assertIn('число столбцов', report['warnings'][0])

    def test_image_table_and_partial_document_have_provenance(self):
        doc = document([dict(type='image', text='|A|B|\n|---|---|\n|1|2|', imagePath='saved.png', pageNumber=8)])
        doc['pendingImages'] = 3
        report = collect_tables(doc)
        self.assertTrue(report['partial'])
        self.assertEqual(report['tables'][0]['origin'], 'recognition')
        self.assertEqual(report['tables'][0]['reference']['imagePath'], 'saved.png')
        self.assertTrue(any('моделью' in w for w in report['warnings']))

    def test_snapshots_change_for_values_and_completion_but_not_dict_order(self):
        doc = document([native([['Код'], ['01']])])
        original = collect_tables(doc)['snapshot']
        self.assertEqual(original, collect_tables(dict(reversed(list(doc.items()))))['snapshot'])
        changed = copy.deepcopy(doc); changed['blocks'][0]['tableRows'][1][0] = '02'
        self.assertNotEqual(original, collect_tables(changed)['snapshot'])
        changed = copy.deepcopy(doc); changed['pendingImages'] = 1
        self.assertNotEqual(original, collect_tables(changed)['snapshot'])

    def test_resource_limits_skip_whole_tables(self):
        doc = document([native([['A', 'B'], ['1', '2']]), native([['Other']])])
        with patch('document_tables.MAX_CELLS', 4):
            report = collect_tables(doc)
        self.assertEqual(len(report['tables']), 1)
        self.assertTrue(report['partial']); self.assertEqual(report['skipped'], 1)
        with patch('document_tables.MAX_BYTES', 3):
            self.assertEqual(collect_tables(document([native([['аб']])]))['tables'], [])

    def test_csv_cyrillic_quotes_multiline_empty_and_formula_safety(self):
        rows = [['Код', 'Значение'], ['00017', 'a;"b"\nline'], ['', '7,5'], ['=1+1', '  @SUM(A1)'], ['-2', '\t=1']]
        data, count = csv_bytes(rows)
        decoded = list(csv.reader(io.StringIO(data.decode('utf-8-sig'), newline=''), delimiter=';'))
        self.assertTrue(data.startswith(b'\xef\xbb\xbf'))
        self.assertEqual(decoded[:3], rows[:3])
        self.assertEqual(count, 4)
        self.assertEqual(decoded[3], ["'=1+1", "'  @SUM(A1)"])

    def test_xlsx_all_values_strings_no_formulas_links_or_macros(self):
        rows = [['Код', 'Значение'], ['00017', '=HYPERLINK("file:///C:/secret")'], ['9007199254740993', '7,5'], ['', 'line\nnext']]
        report = collect_tables(document([native(rows)]))
        data, name, count = export_table(report, '0:0', 'xlsx')
        self.assertTrue(name.endswith('.xlsx')); self.assertEqual(count, 0)
        with ZipFile(io.BytesIO(data)) as z:
            self.assertIsNone(z.testzip())
            for name in z.namelist(): ET.fromstring(z.read(name))
            root = ET.fromstring(z.read('xl/worksheets/sheet1.xml'))
            self.assertFalse(root.findall('.//{' + NS + '}f'))
            self.assertTrue(all(c.attrib['t'] == 's' for c in root.findall('.//{' + NS + '}c')))
            self.assertIn('00017', z.read('xl/sharedStrings.xml').decode())
            self.assertIn('Протокол.docx', z.read('xl/sharedStrings.xml').decode())
            self.assertFalse(any('vba' in name.lower() for name in z.namelist()))

    def test_excel_limits_fail_without_truncation_and_csv_still_works(self):
        for value in ['a' * 32768, 'bad\x01value']:
            report = collect_tables(document([native([[value]])]))
            with self.assertRaises(ValueError): export_table(report, '0:0', 'xlsx')
            data, _, _ = export_table(report, '0:0', 'csv')
            self.assertIn(value, data.decode('utf-8-sig'))
        with self.assertRaises(ValueError): export_table(report, 'missing', 'csv')
        with self.assertRaises(ValueError): export_table(report, '0:0', 'exe')

    def test_xlsx_reopens_with_independent_reader(self):
        try: import openpyxl
        except ImportError: self.skipTest('Independent workbook QA requires openpyxl (bundled test runtime)')
        rows = [['Код', 'Значение'], ['00017', '9007199254740993'], ['=1+1', '  пробелы  '],
                ['line\nnext', 'carriage\rreturn'], ['_x0041_', 'A < B & C'], ['🔧', ''], ['-2', '7,5']]
        report = collect_tables(document([native(rows)]))
        data, _, _ = export_table(report, '0:0', 'xlsx')
        workbook = openpyxl.load_workbook(io.BytesIO(data))
        self.assertEqual(workbook.sheetnames, ['Таблица', 'Источник'])
        actual = [list(row) for row in workbook.worksheets[0].values]
        self.assertEqual(actual, rows)
        self.assertTrue(all(cell.data_type == 's' for row in workbook.worksheets[0] for cell in row))
        self.assertEqual(workbook.worksheets[0].freeze_panes, 'A2')

    def test_real_docx_merged_and_nested_tables(self):
        try: from docx import Document
        except ImportError: self.skipTest('DOCX check requires python-docx')
        sys.path.insert(0, str(ROOT / 'document_tools'))
        from read_docx import extract_document
        folder = ROOT / 'build-qtcreator/test-artifacts'; folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as temporary:
            word = Document(); table = word.add_table(rows=3, cols=2)
            table.cell(0, 0).text = 'Код'; table.cell(0, 1).text = 'Значение'
            table.cell(1, 0).text = '00017'; table.cell(1, 1).text = '7,5 bar'
            table.cell(2, 0).merge(table.cell(2, 1)).text = 'Примечание'
            nested = table.cell(1, 1).add_table(rows=1, cols=2)
            nested.cell(0, 0).text = 'CRC'; nested.cell(0, 1).text = '0x01'
            path = Path(temporary) / 'tables.docx'; word.save(path)
            extracted = extract_document(str(path))
            report = collect_tables(dict(document(extracted['blocks']), text=extracted['text']))
            self.assertEqual(len(report['tables']), 2)
            self.assertEqual(report['tables'][0]['rows'][1][0], '00017')
            self.assertEqual(report['tables'][0]['rows'][2], ['Примечание', ''])
            self.assertEqual(report['tables'][1]['rows'][0], ['CRC', '0x01'])


def benchmark():
    rows = [['Код', 'Значение', 'Единицы', 'Описание']] + [[f'{i:07}', '7,5', 'bar', 'Протокол: давление'] for i in range(20000)]
    start = time.perf_counter(); report = collect_tables(document([native(rows)])); parsed = time.perf_counter()
    data, _, _ = export_table(report, '0:0', 'xlsx')
    print(f'20001 rows / 80004 cells: collect {parsed - start:.3f}s, XLSX {time.perf_counter() - parsed:.3f}s, {len(data)} bytes')


if __name__ == '__main__':
    if '--benchmark' in sys.argv: benchmark()
    else: unittest.main()
