"""Worker protocol: result JSON on stdout, progress lines on stderr."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
from document_format import blocks_to_text
from image_processing import ImageAnalyzer, describe_images
import read_docx
import read_pdf
import read_image


def progress(message):
    print("@@DOCUMENT@@" + json.dumps({"message": message}, ensure_ascii=False),
          file=sys.stderr, flush=True)


def convert_doc(path, directory, soffice=""):
    target = directory / "converted.docx"
    if soffice:
        read_pdf.run_tool([soffice, "-env:UserInstallation=" + (directory / "profile").as_uri(),
                           "--headless", "--convert-to", "docx", "--outdir", directory, path])
        target = directory / (Path(path).stem + ".docx")
    else:
        powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        try:
            read_pdf.run_tool([powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                              "-File", Path(__file__).with_name("convert_doc.ps1"),
                              "-InputPath", path, "-OutputPath", target])
        except Exception as error:
            raise RuntimeError("Для DOC нужен установленный Microsoft Word или путь documents/soffice "
                               f"к LibreOffice в local-assistant.ini. Конвертация не выполнена: {error}") from error
    if not target.is_file():
        raise RuntimeError("Конвертер DOC не создал DOCX")
    return target


def extract_document(args):
    warnings = []
    path = Path(args.path).resolve(strict=True)
    images = args.images_dir.resolve()
    images.mkdir(parents=True, exist_ok=True)
    progress("Чтение документа: " + path.name)
    suffix = path.suffix.lower()
    if suffix == ".pdf":
        blocks = read_pdf.extract_blocks(path, images, args.poppler_dir, warnings, progress)
    elif suffix == ".docx":
        blocks = read_docx.extract_blocks(path, images, warnings)
    elif suffix == ".doc":
        progress("Преобразование DOC в DOCX")
        with tempfile.TemporaryDirectory(prefix="local-assistant-doc-") as directory:
            converted = convert_doc(path, Path(directory), args.soffice)
            blocks = read_docx.extract_blocks(converted, images, warnings)
    elif suffix in read_image.SUPPORTED_SUFFIXES:
        progress("Подготовка изображения: " + path.name)
        blocks = read_image.extract_blocks(path, images)
    else:
        raise ValueError("Поддерживаются PDF, DOC, DOCX и изображения PNG, JPG, JPEG, WEBP, BMP")
    analyzer = (ImageAnalyzer(args.vision_url, "local-chat", args.vision_profile,
                              images / "descriptions") if args.vision_url else None)
    describe_images(blocks, analyzer, warnings, progress)
    text = blocks_to_text(blocks)
    if not text.strip():
        raise ValueError("В документе нет доступного текста; изображения не распознаны. " +
                         "\n".join(warnings[:5]))
    progress("Документ прочитан; подготовка к индексации")
    return {"text": text, "blocks": blocks, "warnings": warnings}


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--images-dir", required=True, type=Path)
    parser.add_argument("--poppler-dir", required=True, type=Path)
    parser.add_argument("--vision-url", default="")
    parser.add_argument("--vision-profile", default="")
    parser.add_argument("--soffice", default="")
    args = parser.parse_args()
    try:
        print(json.dumps(extract_document(args), ensure_ascii=False))
    except Exception as error:
        print(f"Не удалось обработать документ: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
