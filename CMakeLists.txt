cmake_minimum_required(VERSION 3.16)

project(IndustrialAiAssistant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")

find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

add_executable(IndustrialAiAssistant main.cpp
    src/ui/mainwindow.ui
    src/application/mainwindow.cpp
    src/application/mainwindow.h
    src/application/assistant_service.cpp
    src/application/assistant_service.hpp
    src/documents/document.hpp
    src/rag/chunk.h
    src/rag/document_chunker.cpp
    src/rag/document_chunker.h
    src/rag/vector_store.cpp
    src/rag/vector_store.hpp
    src/rag/embedding_client.hpp
    src/rag/embedding_client.cpp
    src/llm/illm_client.hpp
    src/llm/illm_client.cpp
    src/llm/lmstudio_llm_client.hpp
    src/llm/lmstudio_llm_client.cpp
    src/mcp/mcp_client.hpp
    src/mcp/mcp_client.cpp
    src/chat/chat.hpp
    src/chat/chat_manager.hpp
    src/chat/chat_manager.cpp
    src/chat/chat_list_model.hpp
    src/chat/chat_list_model.cpp
    src/io/document_loader.hpp
    src/io/document_loader.cpp
    src/io/document_manager.hpp
    src/io/document_manager.cpp
    src/documents/document_list_model.hpp
    src/documents/document_list_model.cpp
)

target_include_directories(IndustrialAiAssistant PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mcp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/chat"
)

target_link_libraries(IndustrialAiAssistant PRIVATE Qt5::Widgets Qt5::Network)
