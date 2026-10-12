# Установка и настройка

[К README](../README.md)

## Требования

- Windows x64. Проверенная сборка приложения: C++17, Qt 5.7 и MinGW 5.3 (32 бита); внешние серверы моделей и PostgreSQL — 64-битные.
- Python 3.12 или 3.13 x64. Зависимости фиксируются в `requirements.txt` каждого Python-компонента.
- Для ускорения — совместимая NVIDIA GPU и CUDA-сборка llama.cpp. Доступен запуск на CPU.
- Для старых `.doc` — установленный Microsoft Word либо LibreOffice. Вариант с Word проверен локально; путь LibreOffice предусмотрен, но локально не проверен.

## Модели и внешние программы

Подготовьте файлы относительно корня приложения. Модели, локальные окружения и большинство бинарников в Git не хранятся.

| Путь | Содержимое |
|---|---|
| `models/Qwen3-VL-8B-Instruct-Q4_K_M.gguf` | Модель ответов |
| `models/mmproj-Qwen3-VL-8B-Instruct-F16.gguf` | Проектор изображений для этой модели |
| `models/embeddinggemma-300m-qat-Q8_0.gguf` | Модель поисковых векторов |
| `tools/llama-cpp/` | Windows x64 CPU-сборка llama.cpp b11429: `llama-server.exe` и DLL |
| `tools/llama-cpp/cuda/` | CUDA 12.4-сборка той же версии и DLL соответствующего архива cudart |
| `tools/poppler-26.09.0/Library/bin/` | Полный комплект Poppler, включая `pdftohtml.exe`, `pdftoppm.exe` и DLL |
| `tools/postgresql/` | Папки `bin`, `lib`, `share` из [Windows x64 архива PostgreSQL](https://www.enterprisedb.com/download-postgresql-binaries); проверена версия 17.11 |

Два одновременных распознавания требуют поддержки `--kv-unified-per-slot` в llama.cpp; проверена сборка b11429. Без проектора остаётся текстовый чат, распознавание изображений недоступно.

## Окружения Python

Выполните из корня проекта на том компьютере, где будет работать приложение. Для Python 3.12 замените `-3.13` на `-3.12`.

```powershell
py -3.13 -m venv mcp_server/.venv
./mcp_server/.venv/Scripts/python.exe -m pip install -r mcp_server/requirements.txt
py -3.13 -m venv document_tools/.venv
./document_tools/.venv/Scripts/python.exe -m pip install -r document_tools/requirements.txt
py -3.13 -m venv storage_tools/.venv
./storage_tools/.venv/Scripts/python.exe -m pip install -r storage_tools/requirements.txt
```

На компьютере без интернета заранее подготовьте wheels для его версии и разрядности Python. Окружения `.venv` создаются на целевом компьютере. Установка офлайн, по одному вызову для каждого компонента:

```powershell
./mcp_server/.venv/Scripts/python.exe -m pip install --no-index --find-links ./tools/offline-wheels -r mcp_server/requirements.txt
./document_tools/.venv/Scripts/python.exe -m pip install --no-index --find-links ./tools/offline-wheels -r document_tools/requirements.txt
./storage_tools/.venv/Scripts/python.exe -m pip install --no-index --find-links ./tools/offline-wheels -r storage_tools/requirements.txt
```

## Сборка и первый запуск

1. Откройте корневой `CMakeLists.txt` в Qt Creator, выберите комплект Qt 5.7 / MinGW 5.3 и соберите проект. Старый `.pro` не поддерживает все текущие компоненты.
2. Проверьте пути в [local-assistant.ini](../local-assistant.ini). Корень определяется по папкам `models` и `tools/llama-cpp` рядом с `.exe` или выше него; запуск из `build-qtcreator` поддерживается.
3. Освободите порты, если ранее запускали серверы вручную: по умолчанию 8080 — чат, 8081 — embeddings, 55432 — PostgreSQL. Приложение сообщает о конфликте и не завершает чужие процессы.
4. Запустите приложение. Оно само запускает модели, MCP и PostgreSQL. Состояние и журнал доступны по нажатию на индикатор сервисов снизу слева.

PostgreSQL создаёт собственный кластер при первом запуске; системная служба не требуется. При штатном закрытии приложение завершает свои серверы. Windows Job Object также предназначен для их завершения при принудительной остановке приложения.

## Настройки

Параметры [local-assistant.ini](../local-assistant.ini) читаются при запуске.

| Секция / параметр | Назначение |
|---|---|
| `models/backend` | `auto`, `cpu` или `cuda` |
| `models/chatModel`, `embeddingModel`, `chatMmproj` | Пути моделей и проектора |
| `models/chatGpuLayers` | Число слоёв модели ответов на GPU; при нехватке VRAM уменьшите |
| `models/chatKvCacheType` | `f16` или `q8_0`; Q8 включает Flash Attention, на CPU используется F16 |
| `models/mmprojGpu` | Размещение проектора на GPU |
| `models/chatContextSize`, `maxAnswerTokens` | Окно контекста и предельная длина ответа |
| `models/chatPort`, `embeddingPort`, `startupTimeoutMs` | Порты и таймаут запуска моделей |
| `documents/maxFileSizeMiB` | Максимальный размер исходного файла, по умолчанию 100 МиБ |
| `documents/visionParallelRequests` | 1 или 2 запроса распознавания; для двух нужны проектор и контекст от 8192 |
| `documents/soffice` | Путь к LibreOffice для конвертации DOC; иначе используется Word |
| `storage/dataDirectory`, `port` | Папка данных и порт собственного PostgreSQL |

Текущий профиль настроен под RTX 4050 Laptop 6 ГБ: 20 GPU-слоёв, F16, контекст 16384, ответ до 6144 токенов. Это исходная конфигурация для проверки на другом компьютере, а не универсальный оптимум. Лимит ответа допускается от 128 до половины окна. [Результаты замеров](benchmarks.md).

## Данные и перенос

В `storage-data` находятся кластер `postgres`, постоянные копии `images`, учётные данные и журнал. Путь меняется через `storage/dataDirectory`; переменные `LOCAL_ASSISTANT_STORAGE_DIR` и `LOCAL_ASSISTANT_STORAGE_PORT` переопределяют настройки для тестов. Кэш распознавания и вставленных скриншотов находится отдельно, в локальной папке данных приложения.

Для запуска вне Qt Creator разверните Qt DLL и плагины с помощью `windeployqt` из используемого комплекта. Пример для локальной установки Qt; при другом расположении измените пути:

```powershell
cmake -E env --modify PATH=path_list_prepend:C:/Qt/QT/Tools/mingw530_32/bin --modify PATH=path_list_prepend:C:/Qt/QT/5.7/mingw53_32/bin C:/Qt/QT/5.7/mingw53_32/bin/windeployqt.exe --release --compiler-runtime --no-translations ./build-qtcreator/IndustrialAiAssistant.exe
```

Переносимая установка включает `.exe` с Qt DLL/плагинами, `models`, `tools`, `local-assistant.ini`, `document_tools`, `mcp_server`, `storage_tools` с папкой `migrations` и созданными на целевом ПК окружениями. Компилятор и Qt Creator для запуска не нужны. На CPU CUDA-комплект не требуется.

При обновлении закройте приложение, обновите бинарник и изменившиеся исходники Python/зависимости. Сохраните настройки целевого ПК и его `storage-data`: эта папка содержит пользовательские данные. Уже установленные совместимые модели и утилиты повторно переносить не требуется. Миграции БД применяются автоматически. Исходные PDF/DOCX/TXT целиком не копируются в БД — сохранены извлечённые данные, индекс и изображения; оригиналы храните отдельно.
