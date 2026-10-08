"""DOCX body, tables and embedded pictures, with stable table coordinates."""
import argparse
import json
from pathlib import Path
import sys
from docx import Document
from docx.text.paragraph import Paragraph
from docx.table import Table
from docx.oxml.ns import qn
from docx.parts.image import ImagePart
from image_processing import save_normalized_image
from document_format import blocks_to_text


def flush_text_block(blocks, text_parts):
    text = "".join(text_parts)
    if text.strip():
        blocks.append({"type": "text", "text": text, "pageNumber": 0})
    text_parts.clear()


def save_image(paragraph, relationship_id, output_dir):
    part = paragraph.part.related_parts.get(relationship_id)
    if not isinstance(part, ImagePart):
        raise ValueError(f"Недоступно встроенное изображение: {relationship_id}")
    return save_normalized_image(part.blob, output_dir)


def extract_paragraph_blocks(paragraph, images_dir=None, warnings=None):
    warnings = warnings if warnings is not None else []
    blocks, text_parts = [], []
    # Includes legacy VML from DOC conversion; skip duplicated AlternateContent fallback.
    def walk(node):
        tag = node.tag.rsplit("}", 1)[-1]
        if tag == "AlternateContent":
            selected = next((c for c in node if c.tag.endswith("}Choice")), None)
            if selected is None:
                selected = next((c for c in node if c.tag.endswith("}Fallback")), None)
            if selected is not None:
                yield from walk(selected)
        elif tag in ("drawing", "pict"):
            yield node
        elif node.tag == qn("w:t"):
            yield node.text or ""
        elif node.tag == qn("w:tab"):
            yield "\t"
        elif node.tag in (qn("w:br"), qn("w:cr")):
            yield "\n"
        elif node.tag != qn("w:del"):
            for child in node:
                yield from walk(child)

    for item in walk(paragraph._p):
        if isinstance(item, str):
            text_parts.append(item)
        elif images_dir is not None:
            flush_text_block(blocks, text_parts)
            seen = set()
            for node in item.iter():
                if node.tag.rsplit("}", 1)[-1] not in ("blip", "imagedata"):
                    continue
                rid = node.get(qn("r:embed")) or node.get(qn("r:id"))
                if not rid:
                    warnings.append("Внешнее изображение DOCX пропущено: файл не встроен в документ")
                    continue
                if rid in seen:
                    continue
                seen.add(rid)
                try:
                    blocks.append({"type": "image", "text": "", "pageNumber": 0,
                                   "imagePath": save_image(paragraph, rid, images_dir)})
                except Exception as error:
                    warnings.append(f"Изображение DOCX не прочитано: {error}")
            if not seen:
                warnings.append("Графический объект DOCX не содержит доступного растра (возможна диаграмма/фигура)")
    flush_text_block(blocks, text_parts)
    return blocks


def extract_blocks(path, images_dir=None, warnings=None):
    document = Document(path)
    warnings = warnings if warnings is not None else []
    table_counter = 0

    def read_container(container, section=""):
        nonlocal table_counter
        result = []
        for item in container.iter_inner_content():
            if isinstance(item, Paragraph):
                result.extend(extract_paragraph_blocks(item, images_dir, warnings))
            elif isinstance(item, Table):
                table_index = table_counter
                table_counter += 1
                rows, extra, visited = [], [], set()
                for row_index, row in enumerate(item.rows):
                    cells = [""] * row.grid_cols_before
                    for column, cell in enumerate(row.cells, row.grid_cols_before):
                        if cell._tc in visited:
                            cells.append("")
                            continue
                        visited.add(cell._tc)
                        inner = read_container(cell)
                        cells.append("\n".join(b["text"] for b in inner if b["type"] == "text"))
                        for block in inner:
                            if block["type"] != "text":
                                if "tableIndex" not in block:
                                    block.update(tableIndex=table_index, tableRow=row_index, tableColumn=column)
                                extra.append(block)
                    cells.extend([""] * row.grid_cols_after)
                    rows.append(cells)
                result.append({"type": "table", "tableRows": rows,
                               "pageNumber": 0, "tableIndex": table_index})
                result.extend(extra)
        if section:
            for block in result:
                block["section"] = section
        return result

    blocks = read_container(document)
    visited_parts = set()
    for section in document.sections:
        for name, part in (("Колонтитул", section.header), ("Колонтитул", section.footer),
                           ("Колонтитул первой страницы", section.first_page_header),
                           ("Колонтитул первой страницы", section.first_page_footer),
                           ("Колонтитул чётной страницы", section.even_page_header),
                           ("Колонтитул чётной страницы", section.even_page_footer)):
            if part.is_linked_to_previous or part.part.partname in visited_parts:
                continue
            visited_parts.add(part.part.partname)
            blocks.extend(read_container(part, name))
    return blocks


def extract_document(path, images_dir=None):
    warnings = []
    blocks = extract_blocks(path, images_dir, warnings)
    return {"text": blocks_to_text(blocks), "blocks": blocks, "warnings": warnings}


def extract_text(path):
    return extract_document(path)["text"]


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--images-dir", type=Path)
    args = parser.parse_args()
    try:
        result = extract_document(args.path, args.images_dir)
        if not result["blocks"]:
            raise ValueError("В DOCX нет текста или встроенных изображений")
        print(json.dumps(result, ensure_ascii=False) if args.json else result["text"])
    except Exception as error:
        print(f"DOCX extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
