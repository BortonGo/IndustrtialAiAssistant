"""Resume extraction without repeating native parsing; never reuse an edited source."""
import argparse
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'document_tools'))
import read_document
WORK = Path(__file__).resolve().parents[1] / 'build-qtcreator/test-artifacts/imports'
WORK.mkdir(parents=True, exist_ok=True)


class ImportCheckpointTests(unittest.TestCase):
    def test_native_preview_skips_vision_and_full_pass_reuses_extraction(self):
        with tempfile.TemporaryDirectory(dir=WORK) as folder:
            root = Path(folder)
            source = root / 'input.docx'; source.write_bytes(b'document')
            image = root / 'image.png'; image.write_bytes(b'image')
            args = argparse.Namespace(path=source, images_dir=root / 'images', poppler_dir=root,
                vision_url='', vision_profile='', vision_parallel_requests=2, soffice='',
                checkpoint_dir=root / 'checkpoints', native_only=True)
            blocks = [{'type': 'text', 'text': 'Native fact', 'pageNumber': 1},
                      {'type': 'table', 'tableRows': [['Code', 'Value'], ['PUMP', '7.5']], 'pageNumber': 1},
                      {'type': 'image', 'imagePath': str(image), 'text': '', 'pageNumber': 2}]
            with patch.object(read_document.read_docx, 'extract_blocks', return_value=blocks) as parse, \
                    patch.object(read_document, 'describe_images') as describe:
                preview = read_document.extract_document(args)
                self.assertEqual(preview['pendingImages'], 1)
                self.assertEqual([b['type'] for b in preview['blocks']], ['text', 'table'])
                self.assertIn('PUMP\t7.5', preview['text'])
                describe.assert_not_called()
                args.native_only = False
                read_document.extract_document(args)
                self.assertEqual(parse.call_count, 1)
                describe.assert_called_once()

    def test_scan_only_has_empty_preview_but_final_requires_recognition(self):
        with tempfile.TemporaryDirectory(dir=WORK) as folder:
            root = Path(folder)
            source = root / 'input.png'; source.write_bytes(b'image')
            args = argparse.Namespace(path=source, images_dir=root / 'images', poppler_dir=root,
                vision_url='', vision_profile='', vision_parallel_requests=2, soffice='', native_only=True)
            with patch.object(read_document.read_image, 'extract_blocks', return_value=[{'type':'image', 'text':'', 'imagePath':str(source)}]), \
                    patch.object(read_document, 'describe_images'):
                preview = read_document.extract_document(args)
                self.assertEqual(preview['text'], '')
                self.assertEqual(preview['pendingImages'], 1)
                args.native_only = False
                with self.assertRaisesRegex(ValueError, 'нет доступного текста'):
                    read_document.extract_document(args)

    def test_resume_reuses_native_blocks_but_rechecks_images_and_content(self):
        with tempfile.TemporaryDirectory(dir=WORK) as folder:
            root = Path(folder)
            source = root / 'input.docx'
            source.write_bytes(b'first document')
            image = root / 'image.png'
            image.write_bytes(b'image')
            args = argparse.Namespace(path=source, images_dir=root / 'images', poppler_dir=root,
                vision_url='', vision_profile='', vision_parallel_requests=2, soffice='', checkpoint_dir=root / 'checkpoints')
            blocks = [{'type': 'text', 'text': 'Original text', 'pageNumber': 0},
                      {'type': 'image', 'imagePath': str(image), 'pageNumber': 0}]
            with patch.object(read_document.read_docx, 'extract_blocks', side_effect=lambda *a: copy.deepcopy(blocks)) as parse, \
                    patch.object(read_document, 'describe_images') as describe:
                first = read_document.extract_document(args)
                second = read_document.extract_document(args)
                self.assertEqual(first, second)
                self.assertEqual(parse.call_count, 1)
                self.assertEqual(describe.call_count, 2)  # The analyzer's own cache validates model/prompt.
                source.write_bytes(b'second document')
                read_document.extract_document(args)
                self.assertEqual(parse.call_count, 2)
                image.unlink()
                read_document.extract_document(args)
                self.assertEqual(parse.call_count, 3)

    def test_interrupted_vision_keeps_native_checkpoint(self):
        with tempfile.TemporaryDirectory(dir=WORK) as folder:
            root = Path(folder)
            source = root / 'input.docx'
            source.write_bytes(b'document')
            args = argparse.Namespace(path=source, images_dir=root / 'images', poppler_dir=root,
                vision_url='', vision_profile='', vision_parallel_requests=2, soffice='', checkpoint_dir=root / 'checkpoints')
            with patch.object(read_document.read_docx, 'extract_blocks', return_value=[{'type': 'text', 'text': 'Test', 'pageNumber': 0}]) as parse:
                with patch.object(read_document, 'describe_images', side_effect=RuntimeError('interrupted')):
                    with self.assertRaisesRegex(RuntimeError, 'interrupted'):
                        read_document.extract_document(args)
                with patch.object(read_document, 'describe_images'):
                    self.assertEqual(read_document.extract_document(args)['text'], 'Test')
                self.assertEqual(parse.call_count, 1)


if __name__ == '__main__':
    unittest.main()
