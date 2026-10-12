# Проверки

[К README](../README.md) · [Подготовка окружения](setup.md)

Все команды выполняются из корня проекта. Для проверок PostgreSQL нужен комплект `tools/postgresql`; тесты хранилища создают отдельные временные базы. Артефакты находятся в игнорируемой папке `build-qtcreator/test-artifacts`.

## Сборка и CTest

Пример для проверенного комплекта Qt/MinGW. Пути адаптируйте к своей установке; CMake должен быть настроен с этим комплектом.

```powershell
cmake -E env --modify PATH=path_list_prepend:C:/Qt/QT/Tools/mingw530_32/bin cmake -S . -B build-qtcreator -DLOCAL_ASSISTANT_BUILD_TESTS=ON
cmake -E env --modify PATH=path_list_prepend:C:/Qt/QT/Tools/mingw530_32/bin cmake --build build-qtcreator -j4
ctest --test-dir build-qtcreator --output-on-failure
```

Перед CTest нужна успешная сборка **всех обычных целей**, включая тесты. Сам CTest ничего не компилирует и может запустить устаревшие исполняемые файлы.

| Тест | Покрытие |
|---|---|
| `context_builder` | Бюджет контекста, история, источники, фильтрация системных показателей, ошибки и отмена |
| `document_pipeline` | Схема извлечённых данных, чанки, происхождение изображений |
| `document_context` | Выбор документов, раскрытие таблиц, примечания и связанные изображения |
| `hybrid_search` | Смысловой/текстовый поиск, точные обозначения и изоляция документов |

## Python

Обработчики документов, OCR, кэш, поток распознавания, сравнение и таблицы:

```powershell
foreach ($suite in @('test_document_tools.py', 'test_image_stream.py', 'test_import_checkpoint.py', 'test_ocr_routing.py', 'test_document_comparison.py', 'test_document_tables.py')) {
    ./document_tools/.venv/Scripts/python.exe -m unittest discover -s tests -p $suite -v
    if ($LASTEXITCODE -ne 0) { throw "Failed: $suite" }
}
```

Хранилище и память диалогов:

```powershell
./storage_tools/.venv/Scripts/python.exe tests/test_storage.py -v
./storage_tools/.venv/Scripts/python.exe tests/test_conversation_memory.py -v
```

`test_storage.py` проверяет миграции, изоляцию чатов, восстановление индекса, вложения, источники, библиотеку и очистку изображений. Использует отдельный кластер на порту 55434. Часть дополнительных проверок XLSX выполняется при наличии `openpyxl`; это не зависимость приложения.

## Интерфейс и очередь

Эти цели собираются отдельно. Модели не запускаются; UI-тест использует временную PostgreSQL на свободном порту, очередь — на 55435 и локальный HTTP-стенд.

```powershell
cmake -E env --modify PATH=path_list_prepend:C:/Qt/QT/Tools/mingw530_32/bin cmake --build build-qtcreator --target chat_ui_integration_test document_import_queue_test -j4
cmake -E env QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=C:/Qt/QT/5.7/mingw53_32/plugins ./build-qtcreator/chat_ui_integration_test.exe
./build-qtcreator/document_import_queue_test.exe
```

UI-тест охватывает навигацию, удаление, вложения, поток ответов, источники, экспорт, библиотеку и хранилище; PNG-превью сохраняются в `build-qtcreator`. Тест очереди проверяет паузу, продолжение, приоритет вопроса, раннюю публикацию текста и повторное использование индекса.

## Реальные модели

Такие тесты не входят в CTest. Запускайте их при закрытом приложении, чтобы серверы не конкурировали за порты и GPU. Сборка отдельной цели: `cmake --build build-qtcreator --target <имя>` в окружении Qt/MinGW.

| Цель | Запуск и условия |
|---|---|
| `service_hybrid_search_test` | Без аргументов; свои модельные серверы и тестовая БД 55437. Поиск кодов, уточнения, память и библиотека |
| `message_attachments_test` | Путь к тестовой фотографии `PUMP-417 / 7.5 bar`; свои модели и БД 55436 |
| `service_import_test` | Путь к DOCX с изображениями и фразой «Проверочный код проекта: TEXT-7319.»; свои модели и БД 55436 |
| `conversation_memory_test` | URL уже запущенного тестового llama-server, например `http://127.0.0.1:8096/v1/`; без БД |
| `context_relevance_test` | URL чата и путь к тестовой фотографии; необязательно JSON с `question` и `history`; без БД |
| `document_retrieval_test` | URL чата, URL embeddings, путь к JSON-фикстуре с `question`, `documents`, `entries`; без БД |
| `service_pipeline_test`, `chat_context_integration_test` | Путь к документу с `PUMP-417 / 7.5 bar`; обязательно задать отдельные `LOCAL_ASSISTANT_STORAGE_DIR` и `LOCAL_ASSISTANT_STORAGE_PORT` |

Персональные документы и фикстуры с их содержимым не добавляются в Git. [Замеры производительности](benchmarks.md) выполняются отдельно от функциональных проверок.

## Ручной сценарий

1. Создать два чата, загрузить разные документы и проверить, что ответы и источники относятся к своему чату.
2. Прикрепить несколько файлов и скриншот через Ctrl+V; проверить Enter, Shift+Enter, отмену и переключение чата во время ответа.
3. Через «Документы чата» загрузить файл с текстом и картинками, задать вопрос по готовому тексту, приостановить и продолжить обработку.
4. Перезапустить приложение: проверить историю, документы, библиотеку и старые источники. Оригинал для просмотра сохранённого фрагмента не обязателен.
5. Проверить сравнение двух редакций, экспорт таблицы и добавление готового документа из библиотеки в другой чат.
6. Удалить тестовый чат, затем проверить отмену и подтверждение очистки хранилища: другие чаты, библиотека и оригиналы должны сохраниться.
