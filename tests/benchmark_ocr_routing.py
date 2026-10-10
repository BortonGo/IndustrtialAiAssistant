"""Opt-in OCR/vision comparison on generated, ground-truthed images; no application data."""
import argparse
import configparser
from contextlib import ExitStack
import difflib
import json
from pathlib import Path
import re
import sys
import time

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'document_tools'))


def fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 24)
    lines = [
        'Протокол проверки оборудования лаборатории.',
        'Перед началом работы необходимо проверить питание.',
        'Оператор осматривает корпус и соединительные кабели.',
        'При обнаружении повреждений включение запрещено.',
        'Проверка проводится при закрытых защитных крышках.',
        'Результаты измерений записываются в журнал наблюдений.',
        'Давление в системе составляет 7,5 бар при нормальной работе.',
        'Температура воздуха в помещении составляет 23 градуса.',
        'Испытание повторяют после устранения всех замечаний.',
        'Ответственный сотрудник подтверждает результат подписью.',
        'Запрещается менять настройки во время измерения.',
        'После завершения проверки оборудование отключают.',
    ]
    samples = []
    for name in ('russian-page', 'page-number', 'mixed-code', 'table', 'diagram', 'page-with-figure', 'two-columns', 'rotated', 'dark-photo'):
        image = Image.new('RGB', (1000, 1000), 'white')
        draw = ImageDraw.Draw(image)
        text = lines[:]
        if name == 'mixed-code':
            text[4] = 'Устройство PUMP-417 подключено к контроллеру PLC-005.'
        for n, line in enumerate(text):
            if name == 'table':
                draw.text((60, 65 + 45 * n), f'Насос {n + 1}', fill='black', font=font)
                draw.text((430, 65 + 45 * n), f'{n + 1},5 бар', fill='black', font=font)
                draw.text((760, 65 + 45 * n), 'Готов', fill='black', font=font)
            elif name == 'two-columns':
                draw.text((60 if n < 6 else 540, 65 + 45 * (n % 6)), line[:27], fill='black', font=font)
            else:
                draw.text((60, 65 + 45 * n), line, fill='black', font=font)
        if name in ('diagram', 'page-with-figure'):
            draw.rectangle((120, 690, 360, 850), outline='black', width=3)
            draw.rectangle((620, 690, 860, 850), outline='black', width=3)
            draw.line((360, 770, 620, 770), fill='black', width=4)
            draw.text((170, 745), 'Насос', fill='black', font=font)
            draw.text((670, 745), 'Клапан', fill='black', font=font)
        if name == 'diagram':
            draw.rectangle((0, 0, 999, 640), fill='white')
        if name == 'page-number':
            draw.text((470, 920), '42', fill='black', font=font)
            text.append('42')
        if name == 'rotated':
            image = image.rotate(8, fillcolor='white')
        if name == 'dark-photo':
            image = Image.new('RGB', image.size, (70, 90, 60))
        path = directory / (name + '.png')
        image.save(path)
        samples.append({'name': name, 'path': str(path), 'expected': '\n'.join(text),
                        'mustUseVision': name not in ('russian-page', 'page-number')})
    # Self-contained fixture: the user's ignored test-documents folder isn't required.
    image = Image.new('RGB', (900, 360), 'white')
    draw = ImageDraw.Draw(image)
    plate_font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 46)
    draw.rectangle((5, 5, 890, 350), outline='red', width=8)
    for y, line in zip((45, 125, 205), ('DEVICE: PUMP-417', 'PRESSURE: 7.5 bar', 'ROOM: 218')):
        draw.text((35, y), line, fill='black', font=plate_font)
    path = directory / 'plate.png'
    image.save(path)
    samples.append({'name': 'plate', 'path': str(path),
                    'mustUseVision': True, 'expected': 'DEVICE: PUMP-417\nPRESSURE: 7.5 bar\nROOM: 218'})
    return samples


def normalized(text):
    return re.sub(r'\s+', ' ', text).strip()


def main():
    from ocr_routing import recognize_text_page
    from image_processing import ImageAnalyzer
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vision-url')
    parser.add_argument('--start-local-server', action='store_true')
    parser.add_argument('--verify-formats', action='store_true', help='Also import PNG, DOCX and a scan-only PDF without a model')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.start_local_server and args.vision_url:
        parser.error('Choose either an existing endpoint or a private local server')
    with ExitStack() as resources:
        if args.start_local_server:
            from benchmark_image_parallelism import Server
            settings = configparser.ConfigParser()
            settings.read(ROOT / 'local-assistant.ini', encoding='utf-8-sig')
            model = settings['models']
            executable = ROOT / 'tools/llama-cpp/cuda/llama-server.exe'
            embedding = ['--model', str(ROOT / model['embeddingModel']), '--embedding', '--ctx-size', '4096',
                         '--batch-size', '2048', '--ubatch-size', '2048', '--gpu-layers', '99']
            resources.enter_context(Server(executable, embedding, 8097, args.output / 'embedding.log'))
            arguments = ['--model', str(ROOT / model['chatModel']), '--mmproj', str(ROOT / model['chatMmproj']),
                         '--alias', 'local-chat', '--ctx-size', model.get('chatContextSize', '16384'), '--parallel', '1',
                         '--gpu-layers', model.get('chatGpuLayers', '20'), '--jinja', '--reasoning', 'off', '--cache-ram', '0',
                         '--cache-type-k', model.get('chatKvCacheType', 'f16'),
                         '--cache-type-v', model.get('chatKvCacheType', 'f16'), '--image-max-tokens', '1024']
            if model.get('chatKvCacheType', 'f16') == 'q8_0':
                arguments += ['--flash-attn', 'on']
            if not model.getboolean('mmprojGpu', True):
                arguments += ['--no-mmproj-offload']
            resources.enter_context(Server(executable, arguments, 8096, args.output / 'vision.log'))
            args.vision_url = 'http://127.0.0.1:8096/v1/chat/completions'
            (args.output / 'settings.json').write_text(json.dumps(dict(model), indent=2), encoding='utf-8')
        run(args, recognize_text_page, ImageAnalyzer)
        if args.verify_formats:
            verify_formats(args.output)


def verify_formats(output):
    from docx import Document
    from docx.shared import Inches
    from read_document import extract_document
    picture = output / 'images/russian-page.png'
    document = Document()
    document.add_picture(str(picture), width=Inches(6))
    document.save(output / 'text-scan.docx')
    with Image.open(picture) as image:
        image.convert('RGB').save(output / 'text-scan.pdf', 'PDF', resolution=120)
    report = []
    for path in (picture, output / 'text-scan.docx', output / 'text-scan.pdf'):
        options = argparse.Namespace(path=path, images_dir=output / 'pipeline-images',
            poppler_dir=ROOT / 'tools/poppler-26.09.0/Library/bin', vision_url='', vision_profile='',
            vision_parallel_requests=1, soffice='', checkpoint_dir=output / 'checkpoints', native_only=False)
        started = time.perf_counter()
        result = extract_document(options)
        images = [b for b in result['blocks'] if b['type'] == 'image']
        if not images or not all(b.get('recognitionMethod') == 'windows-ocr' for b in images):
            raise AssertionError('OCR import failed for ' + str(path))
        if result['warnings']:
            raise AssertionError(result['warnings'])
        report.append(dict(format=path.suffix, seconds=time.perf_counter() - started, blocks=len(result['blocks'])))
    (output / 'formats.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('PNG, DOCX and scan-only PDF imports passed without vision', flush=True)


def run(args, recognize_text_page, ImageAnalyzer):
    report = []
    for item in fixtures(args.output / 'images'):
        diagnostics = {}
        start = time.perf_counter()
        text = recognize_text_page(item['path'], diagnostics)
        result = dict(item, ocrSeconds=time.perf_counter() - start, ocrText=text, diagnostics=diagnostics)
        if text:
            result['textSimilarity'] = difflib.SequenceMatcher(None, normalized(item['expected']), normalized(text), autojunk=False).ratio()
        if text and item['mustUseVision']:
            raise AssertionError('Unsafe OCR acceptance: ' + item['name'])
        if text and result['textSimilarity'] < .99:
            raise AssertionError('OCR quality regression: ' + item['name'])
        if args.vision_url and item['name'] in ('russian-page', 'mixed-code', 'diagram'):
            analyzer = ImageAnalyzer(args.vision_url, 'local-chat', str(time.time_ns()), args.output / 'vision-cache', seed=42)
            start = time.perf_counter()
            vision, truncated = analyzer.describe(item['path'])
            result.update(visionSeconds=time.perf_counter() - start, visionText=vision, visionTruncated=truncated)
        report.append(result)
        (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(item['name'], round(result['ocrSeconds'], 3), diagnostics, flush=True)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
