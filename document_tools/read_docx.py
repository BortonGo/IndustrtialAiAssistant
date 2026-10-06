from docx import Document
from docx.text.paragraph import Paragraph
from docx.table import Table
import sys

def extract_text(path: str) -> str :
    document = Document(path)
    parts = []

    for block in document.iter_inner_content():
        if (isinstance(block, Paragraph)):
            if block.text.strip():
                parts.append(block.text)
        elif (isinstance(block, Table)) :
            for row in block.rows:
                row_text = "\t".join(cell.text for cell in row.cells)
                if row_text.strip() :
                    parts.append(row_text)

    return "\n".join(parts)

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

    if len(sys.argv) != 2:
        print("Usage: read_docx.py <file.docx>", file=sys.stderr)
        sys.exit(1)

    try:
        result = extract_text(sys.argv[1])

        if (not result.strip()):
            print("Empty result after strip()", file=sys.stderr)
            sys.exit(1)

        print(result)
        
    except Exception as error:
        print(f"DOCX extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
