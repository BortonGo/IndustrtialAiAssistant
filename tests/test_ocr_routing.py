"""Routing, failure isolation, cache and provenance regressions; no model/GPU required."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from urllib.error import URLError

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'document_tools'))
from document_format import blocks_to_text
from image_processing import describe_images
from ocr_routing import recognize_text_page, text_page


def page():
    image = Image.new('RGB', (1000, 1000), 'white')
    draw = ImageDraw.Draw(image)
    lines = []
    for row in range(12):
        words = []
        for column, text in enumerate('Проверка оборудования выполняется после завершения работы системы'.split()):
            x, y = 40 + column * 130, 60 + row * 40
            word = dict(text=text, x=x, y=y, width=120, height=20)
            words.append(word)
            draw.rectangle((x, y, x + 115, y + 18), fill='black')
        lines.append(dict(text=' '.join(w['text'] for w in words), words=words))
    return image, dict(lines=lines, angle=0)


class OcrRoutingTests(unittest.TestCase):
    def setUp(self):
        artifacts = ROOT / 'build-qtcreator/test-artifacts/ocr'
        artifacts.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=artifacts)
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.image, self.result = page()
        self.path = self.work / 'page.png'
        self.image.save(self.path)

    def test_complete_prose_preserves_lines_and_real_repetitions(self):
        expected = '\n'.join(l['text'] for l in self.result['lines'])
        details = {}
        self.assertEqual(text_page(self.image, self.result, details), expected)
        self.assertEqual(details['reason'], 'plain-text-page')

    def test_unexplained_figure_or_missed_line_prevents_fast_path(self):
        draw = ImageDraw.Draw(self.image)
        draw.rectangle((100, 650, 900, 850), outline='black', width=5)
        details = {}
        self.assertIsNone(text_page(self.image, self.result, details))
        self.assertEqual(details['reason'], 'graphics-or-missed-text')
        image, result = page()
        result['lines'].pop(3)
        self.assertIsNone(text_page(image, result, {}))

    def test_heading_page_number_preserved_but_colored_figure_rejected(self):
        result = copy.deepcopy(self.result)
        result['lines'].append(dict(text='42', words=[dict(text='42', x=500, y=920, width=30, height=20)]))
        self.assertTrue(text_page(self.image, result, {}).endswith('\n42'))
        ImageDraw.Draw(self.image).rectangle((100, 700, 900, 800), outline='yellow', width=5)
        self.assertIsNone(text_page(self.image, result, {}))

    def test_tables_columns_rotation_tiny_text_and_identifiers_go_to_vision(self):
        variants = []
        for change in ('columns', 'rotation', 'tiny', 'latin', 'code', 'nan', 'order'):
            result = copy.deepcopy(self.result)
            if change == 'columns':
                result['lines'][3]['words'][2]['x'] += 80
            elif change == 'rotation':
                result['angle'] = 6
            elif change == 'tiny':
                result['lines'][0]['words'][0]['height'] = 8
            elif change == 'latin':
                result['lines'][0]['text'] += ' PUMP'
            elif change == 'code':
                result['lines'][0]['words'][0]['text'] = 'ИМР-417'
            elif change == 'nan':
                result['lines'][0]['words'][0]['x'] = float('nan')
            elif change == 'order':
                result['lines'].reverse()
            variants.append((change, result))
        for name, result in variants:
            with self.subTest(name=name):
                self.assertIsNone(text_page(self.image, result, {}))

    def test_dark_photo_never_starts_ocr(self):
        Image.new('RGB', (200, 200), (90, 100, 110)).save(self.path)
        with patch('ocr_routing.subprocess.run') as run:
            self.assertIsNone(recognize_text_page(self.path))
        run.assert_not_called()

    def test_successful_cache_tracks_pixels_and_marks_origin(self):
        completed = SimpleNamespace(returncode=0, stdout=json.dumps(self.result).encode())
        with patch('ocr_routing.subprocess.run', return_value=completed) as run:
            first = recognize_text_page(self.path, cache=self.work / 'cache')
            self.assertTrue(first)
            details = {}
            self.assertEqual(recognize_text_page(self.path, details, self.work / 'cache'), first)
            self.assertEqual((details['method'], details['cacheHit']), ('windows-ocr', True))
            self.assertEqual(run.call_count, 1)
            self.image.putpixel((999, 999), (245, 245, 245))
            self.image.save(self.path)
            recognize_text_page(self.path, cache=self.work / 'cache')
            self.assertEqual(run.call_count, 2)

    def test_missing_language_timeout_invalid_json_fall_back_without_cache(self):
        for response in (SimpleNamespace(returncode=1, stdout=b'{}'),
                         SimpleNamespace(returncode=0, stdout=b'not json'),
                         SimpleNamespace(returncode=0, stdout=b'{"lines":null}'),
                         subprocess.TimeoutExpired('ocr', 20), FileNotFoundError('powershell')):
            with self.subTest(response=response):
                kwargs = {'side_effect': response} if isinstance(response, Exception) else {'return_value': response}
                details = {}
                with patch('ocr_routing.subprocess.run', **kwargs):
                    self.assertIsNone(recognize_text_page(self.path, details, cache=self.work / 'cache'))
                if isinstance(response, subprocess.TimeoutExpired):
                    self.assertEqual(details['reason'], 'windows-ocr-unavailable')
                self.assertFalse(list((self.work / 'cache').glob('*')))

    def test_pipeline_preserves_order_origin_and_duplicate_images(self):
        analyzer = SimpleNamespace(describe=Mock(return_value=('Схема насоса', False)))
        blocks = [dict(type='image', imagePath=p, text='') for p in ('page', 'scheme', 'page')]
        with patch('ocr_routing.recognize_text_page', side_effect=lambda p, *args: 'Распознанная страница' if p == 'page' else None):
            warnings, progress = [], []
            describe_images(blocks, analyzer, warnings, progress.append, 2, use_ocr=True)
        analyzer.describe.assert_called_once_with('scheme')
        self.assertFalse(warnings)
        self.assertEqual([b.get('recognitionMethod') for b in blocks], ['windows-ocr', None, 'windows-ocr'])
        self.assertIn('OCR: 2', progress[-1])
        text = blocks_to_text(blocks)
        self.assertEqual(text.count('[Текст изображения распознан OCR; возможны ошибки]'), 2)
        self.assertEqual(text.count('[Описание изображения моделью; возможны ошибки]'), 1)

    def test_ocr_can_finish_without_vision_server(self):
        analyzer = SimpleNamespace(describe=Mock(side_effect=URLError('server offline')))
        blocks = [dict(type='image', imagePath=p, text='') for p in ('scheme', 'page', 'other')]
        with patch('ocr_routing.recognize_text_page', side_effect=lambda p, *args: 'Текст' if p == 'page' else None):
            warnings = []
            describe_images(blocks, analyzer, warnings, lambda text: None, use_ocr=True)
        self.assertEqual([b['text'] for b in blocks], ['', 'Текст', ''])
        self.assertEqual(len(warnings), 2)
        analyzer.describe.assert_called_once()

    def test_missing_ocr_is_probed_once_per_document(self):
        def missing(path, details, cache):
            details['reason'] = 'windows-ocr-unavailable'
        analyzer = SimpleNamespace(describe=Mock(return_value=('Модель', False)))
        blocks = [dict(type='image', imagePath=p, text='') for p in ('one', 'two', 'three')]
        with patch('ocr_routing.recognize_text_page', side_effect=missing) as ocr:
            describe_images(blocks, analyzer, [], lambda text: None, use_ocr=True)
        ocr.assert_called_once()
        self.assertEqual(analyzer.describe.call_count, 3)


if __name__ == '__main__':
    unittest.main()
