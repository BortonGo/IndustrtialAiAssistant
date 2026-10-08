"""PDF extraction with the existing Poppler distribution."""
import io
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from PIL import Image, ImageChops
from image_processing import save_normalized_image


def run_tool(arguments, timeout=180):
    result = subprocess.run([str(arg) for arg in arguments], capture_output=True, timeout=timeout,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace").strip() or
                           f"{Path(arguments[0]).name}: exit {result.returncode}")
    return result


def extract_blocks(path, images_dir, poppler_dir, warnings, progress):
    blocks = []
    with tempfile.TemporaryDirectory(prefix="local-assistant-pdf-") as temporary:
        work = Path(temporary)
        # XML preserves the page number, reading order and image rectangles.
        xml_path = work / "document.xml"
        progress("PDF: извлечение текста и расположения изображений")
        run_tool([poppler_dir / "pdftohtml.exe", "-xml", "-hidden", "-enc", "UTF-8",
                  "-zoom", "1", "-fmt", "png", path, xml_path])
        pages = ET.parse(xml_path).getroot().findall("page")
        for page in pages:
            number = int(page.attrib["number"])
            progress(f"PDF: страница {number}/{len(pages)}")
            text = "\n".join("".join(node.itertext()) for node in page.findall("text")).strip()
            if text:
                blocks.append({"type": "text", "text": text, "pageNumber": number})
            pictures = page.findall("image")
            if not pictures and text:
                continue
            try:
                # Render the visible page instead of raw embedded pixels: this preserves
                # transparency masks, rotation and clipped images, including scanned PDFs.
                prefix = work / "render"
                run_tool([poppler_dir / "pdftoppm.exe", "-f", number, "-l", number,
                          "-singlefile", "-scale-to", "1800", "-cropbox", "-png", path, prefix])
                with Image.open(prefix.with_suffix(".png")) as source:
                    rendered = source.convert("RGB")
                width, height = float(page.attrib["width"]), float(page.attrib["height"])
                scan = len(text) < 40 or any(
                    abs(float(pic.attrib["width"]) * float(pic.attrib["height"])) > width * height * .6
                    for pic in pictures)
                regions = [None] if scan else pictures
                if not regions:
                    regions = [None]
                seen = set()
                for picture in regions:
                    image = rendered
                    if picture is not None:
                        x, y = float(picture.attrib["left"]), float(picture.attrib["top"])
                        w, h = float(picture.attrib["width"]), float(picture.attrib["height"])
                        sx, sy = rendered.width / width, rendered.height / height
                        # Poppler may express rotation / mirroring as negative dimensions.
                        bounds = (max(0, int(min(x, x + w) * sx)), max(0, int(min(y, y + h) * sy)),
                                  min(rendered.width, int(max(x, x + w) * sx + 1)),
                                  min(rendered.height, int(max(y, y + h) * sy + 1)))
                        if bounds[2] <= bounds[0] or bounds[3] <= bounds[1]:
                            warnings.append(f"PDF, страница {number}: изображение вне видимой области")
                            continue
                        image = rendered.crop(bounds)
                    if ImageChops.difference(image, Image.new("RGB", image.size, "white")).getbbox() is None:
                        continue  # Truly blank pages do not need model inference.
                    stream = io.BytesIO()
                    image.save(stream, format="PNG")
                    image_path = save_normalized_image(stream.getvalue(), images_dir)
                    if image_path not in seen:
                        blocks.append({"type": "image", "text": "", "imagePath": image_path,
                                       "pageNumber": number})
                        seen.add(image_path)
            except Exception as error:
                warnings.append(f"PDF, страница {number}: изображение не извлечено: {error}")
    return blocks
