"""Conservative local OCR fast path. Ambiguous layouts always return to vision."""
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

ROUTING_VERSION = 'windows-text-page-v1'


def text_page(image, result, diagnostics):
    """Only accept dense single-column prose with almost no unexplained visible ink.

    Windows OCR has no confidence score. These gates check layout/coverage, not
    semantic correctness; the original image and OCR provenance remain available.
    """
    def reject(reason):
        diagnostics['reason'] = reason
        return None

    width, height = image.size
    lines = result['lines']
    if not 8 <= len(lines) <= 100 or abs(result.get('angle') or 0) > 2:
        return reject('not-straight-prose')
    words = [word for line in lines for word in line['words']]
    if not 65 <= len(words) <= 2000:
        return reject('too-little-text')
    text = '\n'.join(line['text'] for line in lines)
    letters = [c for c in text if c.isalpha()]
    # The Russian engine can turn Latin device codes into Cyrillic lookalikes.
    # Keep mixed-language/technical pages on vision until separately validated.
    if not letters or any(not ('А' <= c <= 'я' or c in 'Ёё') for c in letters):
        return reject('mixed-language')
    for word in words:
        token = word['text']
        if any(c.isdigit() for c in token) and any(c.isalpha() for c in token):
            return reject('technical-identifier')
        bounds = [word[k] for k in ('x', 'y', 'width', 'height')]
        if not all(isinstance(v, (float, int)) and math.isfinite(v) for v in bounds):
            return reject('invalid-bounds')
        x, y, w, h = bounds
        if w <= 0 or h < 12 or x < 0 or y < 0 or x + w > width + 1 or y + h > height + 1:
            return reject('invalid-or-small-text')
    typical_height = statistics.median(w['height'] for w in words)
    starts, centers = [], []
    for index, line in enumerate(lines):
        row = line['words']
        if len(row) < 4:
            # A short heading or printed page number is normal; scattered labels aren't.
            if index == 0 or (index == len(lines) - 1 and line['text'].strip().isdigit()):
                continue
            return reject('short-lines')
        starts.append(row[0]['x'])
        centers.append(statistics.median(w['y'] + w['height'] / 2 for w in row))
        for previous, word in zip(row, row[1:]):
            gap = word['x'] - previous['x'] - previous['width']
            if gap < -2 or gap > typical_height * 2:
                return reject('columns-or-table')
    if max(starts) - min(starts) > typical_height * 2:
        return reject('unaligned-lines')
    if any(b - a < typical_height * .8 or b - a > typical_height * 3.5
           for a, b in zip(centers, centers[1:])):
        return reject('fragmented-layout')
    red, green, blue = image.convert('RGB').split()
    ink = ImageChops.darker(ImageChops.darker(red, green), blue).point(lambda p: 255 if p < 225 else 0)
    total_ink = ImageStat.Stat(ink).sum[0]
    mask = ImageDraw.Draw(ink)
    for word in words:
        pad = min(3, typical_height * .12)
        x, y = word['x'], word['y']
        mask.rectangle((x - pad, y - pad, x + word['width'] + pad, y + word['height'] + pad), fill=0)
    leftover = ImageStat.Stat(ink).sum[0] / max(total_ink, 1)
    diagnostics['unexplainedInk'] = round(leftover, 4)
    if leftover > .015:
        return reject('graphics-or-missed-text')
    diagnostics['reason'] = 'plain-text-page'
    return text


def recognize_text_page(path, diagnostics=None, cache=None):
    details = diagnostics if diagnostics is not None else {}
    details.update(method='vision', cacheHit=False)
    if sys.platform != 'win32':
        details['reason'] = 'windows-ocr-unavailable'
        return None
    try:
        with Image.open(path) as source:
            image = source.convert('RGB')
        # Cheap rejection before starting PowerShell: photos/dark UI are not scan pages.
        histogram = image.convert('L').histogram()
        if sum(histogram[230:]) < image.width * image.height * .80:
            details['reason'] = 'not-white-page'
            return None
        cache_file = None
        if cache is not None:
            identity = Path(path).read_bytes() + (ROUTING_VERSION + str(sys.getwindowsversion())).encode()
            cache_file = Path(cache) / (hashlib.sha256(identity).hexdigest() + '.json')
            try:
                saved = json.loads(cache_file.read_text(encoding='utf-8'))
                if isinstance(saved['text'], str) and saved['text'].strip():
                    details.update(method='windows-ocr', cacheHit=True, reason='plain-text-page')
                    return saved['text']
            except (OSError, ValueError, KeyError, TypeError):
                pass
        powershell = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        completed = subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', str(Path(__file__).with_name('windows_ocr.ps1')), '-ImagePath', str(Path(path).resolve())],
            capture_output=True, timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if completed.returncode:
            details['reason'] = 'windows-ocr-unavailable'
            return None
        result = json.loads(completed.stdout.decode('utf-8-sig'))
        text = text_page(image, result, details)
        if text:
            details['method'] = 'windows-ocr'
            if cache_file:
                # Imported lazily: image_processing also invokes this module.
                from image_processing import atomic_write
                atomic_write(cache_file, json.dumps({'text': text}, ensure_ascii=False).encode('utf-8'))
        return text
    except subprocess.TimeoutExpired:
        # A stuck platform component must not add 20 seconds to every following image.
        details.update(reason='windows-ocr-unavailable', error='TimeoutExpired')
        return None
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        details.update(reason='ocr-fallback', error=type(error).__name__)
        return None
