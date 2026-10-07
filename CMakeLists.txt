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
    src/ui/mainwindow.ui
    src/application/mainwindow.cpp
    include/application/mainwindow.h
    src/application/assistant_service.cpp
    include/application/assistant_service.hpp
    include/documents/document.hpp
    include/rag/chunk.h
    src/rag/document_chunker.cpp
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
    src/ui/sidebar_widget.cpp
    include/ui/chat_widget.hpp
    src/ui/chat_widget.cpp
    include/ui/message_widget.hpp
    src/ui/message_widget.cpp
    resources/resources.qrc
    include/io/idocument_reader.hpp
    src/io/idocument_reader.cpp
    include/io/pdf_text_extractor.hpp
    src/io/pdf_text_extractor.cpp
    include/io/txt_document_reader.hpp
    src/io/txt_document_reader.cpp
    include/io/pdf_document_reader.hpp
    src/io/pdf_document_reader.cpp
    include/io/docx_document_reader.hpp
    src/io/docx_document_reader.cpp
    include/io/docx_text_extractor.hpp
    src/io/docx_text_extractor.cpp
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
