"""A standalone photo uses the same image blocks and vision cache as documents."""
import io
from pathlib import Path

from PIL import Image, UnidentifiedImageError

from image_processing import save_normalized_image

SUPPORTED_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".bmp"}


def extract_blocks(path: Path, images_dir: Path) -> list[dict]:
    try:
        data = Path(path).read_bytes()
        with Image.open(io.BytesIO(data)) as image:
            if getattr(image, "is_animated", False):
                raise ValueError("Анимированные изображения не поддерживаются; сохраните отдельный кадр")
        image_path = save_normalized_image(data, images_dir)
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError) as error:
        raise ValueError(f"Не удалось прочитать изображение: {error}") from error
    return [{"type": "image", "text": "", "imagePath": image_path, "pageNumber": 0}]
