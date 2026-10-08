"""The text projection is shared by JSON output and C++ chunk offsets."""


def block_text(block):
    if block["type"] == "table":
        return "\n".join("\t".join(row) for row in block["tableRows"]
                         if "\t".join(row).strip())
    text = block.get("text", "")
    if block["type"] == "image" and text.strip():
        return "[Описание изображения моделью; возможны ошибки]\n" + text
    return text


def blocks_to_text(blocks):
    return "\n".join(text for block in blocks if (text := block_text(block)).strip())
