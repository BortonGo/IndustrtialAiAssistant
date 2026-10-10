cmake_minimum_required(VERSION 3.16)

project(IndustrialAiAssistant LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")

find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

add_library(md4c STATIC
    third_party/md4c/md4c.c
    third_party/md4c/md4c-html.c
    third_party/md4c/entity.c
)

target_include_directories(md4c PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c"
)

target_compile_definitions(md4c PRIVATE MD4C_USE_UTF8)

set_target_properties(md4c PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

add_executable(IndustrialAiAssistant src/main.cpp
    include/application/document_import_queue.hpp src/application/document_import_queue.cpp
    include/llm/context_builder.hpp src/llm/context_builder.cpp
    include/llm/context_selection.hpp src/llm/context_selection.cpp
    include/llm/system_status_policy.hpp src/llm/system_status_policy.cpp
    include/llm/completion_stream.hpp src/llm/completion_stream.cpp
    include/ui/attachment_card.hpp src/ui/attachment_card.cpp
    include/ui/source_panel.hpp src/ui/source_panel.cpp
    include/ui/document_comparison_dialog.hpp src/ui/document_comparison_dialog.cpp
    include/storage/storage_client.hpp src/storage/storage_client.cpp
    include/storage/storage_codec.hpp src/storage/storage_codec.cpp
    src/ui/mainwindow.ui
    src/application/mainwindow.cpp
    include/application/mainwindow.h
    src/application/assistant_service.cpp
    include/application/assistant_service.hpp
    include/documents/document.hpp
    include/rag/chunk.h
    src/rag/document_chunker.cpp
    include/rag/document_context.hpp src/rag/document_context.cpp
    include/rag/document_chunker.h
    src/rag/vector_store.cpp
    include/rag/vector_store.hpp
    include/rag/embedding_client.hpp
    src/rag/embedding_client.cpp
    include/llm/illm_client.hpp
    src/llm/illm_client.cpp
    include/llm/lmstudio_llm_client.hpp
    src/llm/lmstudio_llm_client.cpp
    include/mcp/mcp_client.hpp
    src/mcp/mcp_client.cpp
    include/chat/chat.hpp
    include/chat/chat_manager.hpp
    src/chat/chat_manager.cpp
    include/chat/chat_list_model.hpp
    src/chat/chat_list_model.cpp
    include/io/document_loader.hpp
    src/io/document_loader.cpp
    include/io/document_manager.hpp
    src/io/document_manager.cpp
    include/documents/document_list_model.hpp
    src/documents/document_list_model.cpp
    include/ui/documents_widget.hpp
    src/ui/documents_widget.cpp
    include/ui/message_composer.hpp
    src/ui/message_composer.cpp
    include/ui/sidebar_widget.hpp
    include/ui/ui_style.hpp
    src/ui/sidebar_widget.cpp
    include/ui/chat_widget.hpp
    src/ui/chat_widget.cpp
    include/ui/thinking_indicator.hpp
    src/ui/thinking_indicator.cpp
    include/ui/message_widget.hpp
    src/ui/message_widget.cpp
    resources/resources.qrc
    include/io/idocument_reader.hpp
    src/io/idocument_reader.cpp
    include/io/txt_document_reader.hpp
    src/io/txt_document_reader.cpp
    include/io/document_extractor.hpp
    src/io/document_extractor.cpp
    src/io/document_content_parser.cpp
    include/io/structured_document_reader.hpp
    src/io/structured_document_reader.cpp
    include/ui/markdown_render.hpp
    src/ui/markdown_render.cpp
    include/llm/model_api_config.hpp
    include/llm/llama_server_config.hpp
    include/llm/llama_server_process.hpp
    src/llm/llama_server_process.cpp
    include/application/runtime_paths.hpp
    src/application/runtime_paths.cpp
    include/ui/server_status_widget.hpp
    src/ui/server_status_widget.cpp
    include/documents/document_block.hpp
)

target_include_directories(IndustrialAiAssistant PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rag"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/llm"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/mcp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/chat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/io"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/documents"
)

target_link_libraries(IndustrialAiAssistant PRIVATE
    Qt5::Widgets
    Qt5::Network
    md4c
)

if(WIN32)
    target_link_libraries(IndustrialAiAssistant PRIVATE dwmapi)
    enable_language(RC)
    target_sources(IndustrialAiAssistant PRIVATE resources/app.rc)
    target_include_directories(IndustrialAiAssistant PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    )
endif()

option(LOCAL_ASSISTANT_BUILD_TESTS "Build local pipeline and context checks" OFF)
if(LOCAL_ASSISTANT_BUILD_TESTS)
    enable_testing()
    add_executable(context_builder_test tests/context_builder_test.cpp src/llm/context_builder.cpp
        src/llm/context_selection.cpp src/llm/system_status_policy.cpp src/llm/completion_stream.cpp src/llm/lmstudio_llm_client.cpp src/llm/illm_client.cpp
        include/llm/context_builder.hpp include/llm/illm_client.hpp)
    target_include_directories(context_builder_test PRIVATE include include/llm)
    target_link_libraries(context_builder_test PRIVATE Qt5::Core Qt5::Network)
    add_test(NAME context_builder COMMAND context_builder_test)
    add_executable(context_relevance_test EXCLUDE_FROM_ALL tests/context_relevance_test.cpp
        src/llm/context_builder.cpp src/llm/context_selection.cpp src/llm/system_status_policy.cpp src/llm/completion_stream.cpp src/llm/lmstudio_llm_client.cpp
        src/llm/illm_client.cpp include/llm/context_builder.hpp include/llm/illm_client.hpp)
    target_include_directories(context_relevance_test PRIVATE include include/llm)
    target_link_libraries(context_relevance_test PRIVATE Qt5::Core Qt5::Network)
    add_executable(document_pipeline_test tests/document_pipeline_test.cpp
        src/storage/storage_codec.cpp
        src/io/document_manager.cpp include/io/document_manager.hpp
        src/io/document_content_parser.cpp src/io/document_extractor.cpp
        include/io/document_extractor.hpp src/rag/document_chunker.cpp
        src/io/document_loader.cpp include/io/document_loader.hpp
        src/io/structured_document_reader.cpp include/io/structured_document_reader.hpp
        src/io/txt_document_reader.cpp include/io/txt_document_reader.hpp
        src/io/idocument_reader.cpp include/io/idocument_reader.hpp)
    target_include_directories(document_pipeline_test PRIVATE include include/io include/rag)
    target_link_libraries(document_pipeline_test PRIVATE Qt5::Core)
    add_test(NAME document_pipeline COMMAND document_pipeline_test)
    add_executable(document_context_test tests/document_context_test.cpp src/rag/document_context.cpp
        src/rag/vector_store.cpp src/llm/context_selection.cpp)
    target_include_directories(document_context_test PRIVATE include include/rag include/llm)
    target_link_libraries(document_context_test PRIVATE Qt5::Core)
    add_test(NAME document_context COMMAND document_context_test)
    add_executable(hybrid_search_test tests/hybrid_search_test.cpp src/rag/vector_store.cpp src/rag/document_context.cpp)
    target_include_directories(hybrid_search_test PRIVATE include include/rag)
    target_link_libraries(hybrid_search_test PRIVATE Qt5::Core)
    add_test(NAME hybrid_search COMMAND hybrid_search_test)
    add_executable(document_retrieval_test EXCLUDE_FROM_ALL tests/document_retrieval_test.cpp
        src/rag/document_context.cpp src/rag/vector_store.cpp src/rag/embedding_client.cpp include/rag/embedding_client.hpp
        src/storage/storage_codec.cpp src/io/document_content_parser.cpp
        src/llm/context_builder.cpp src/llm/context_selection.cpp src/llm/system_status_policy.cpp
        src/llm/completion_stream.cpp src/llm/lmstudio_llm_client.cpp src/llm/illm_client.cpp include/llm/context_builder.hpp include/llm/illm_client.hpp)
    target_include_directories(document_retrieval_test PRIVATE include include/rag include/llm include/io)
    target_link_libraries(document_retrieval_test PRIVATE Qt5::Core Qt5::Network)
    # Built on demand because this test starts the actual configured model servers.
    get_target_property(APP_TEST_SOURCES IndustrialAiAssistant SOURCES)
    get_target_property(APP_TEST_INCLUDES IndustrialAiAssistant INCLUDE_DIRECTORIES)
    list(REMOVE_ITEM APP_TEST_SOURCES src/main.cpp)
    add_executable(service_hybrid_search_test EXCLUDE_FROM_ALL tests/service_hybrid_search_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(service_hybrid_search_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(service_hybrid_search_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(service_hybrid_search_test PRIVATE dwmapi)
    endif()
    add_executable(service_import_test EXCLUDE_FROM_ALL tests/service_import_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(service_import_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(service_import_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(service_import_test PRIVATE dwmapi)
    endif()
    add_executable(document_import_queue_test EXCLUDE_FROM_ALL tests/document_import_queue_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(document_import_queue_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(document_import_queue_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(document_import_queue_test PRIVATE dwmapi)
    endif()
    add_executable(service_pipeline_test EXCLUDE_FROM_ALL tests/service_pipeline_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(service_pipeline_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(service_pipeline_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(service_pipeline_test PRIVATE dwmapi)
    endif()
    add_executable(chat_context_integration_test EXCLUDE_FROM_ALL tests/chat_context_integration_test.cpp ${APP_TEST_SOURCES})
    add_executable(message_attachments_test EXCLUDE_FROM_ALL tests/message_attachments_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(message_attachments_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(message_attachments_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(message_attachments_test PRIVATE dwmapi)
    endif()
    target_include_directories(chat_context_integration_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(chat_context_integration_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(chat_context_integration_test PRIVATE dwmapi)
    endif()
    add_executable(chat_ui_integration_test EXCLUDE_FROM_ALL tests/chat_ui_integration_test.cpp ${APP_TEST_SOURCES})
    target_include_directories(chat_ui_integration_test PRIVATE ${APP_TEST_INCLUDES})
    target_link_libraries(chat_ui_integration_test PRIVATE Qt5::Widgets Qt5::Network md4c)
    if(WIN32)
        target_link_libraries(chat_ui_integration_test PRIVATE dwmapi)
    endif()
endif()
