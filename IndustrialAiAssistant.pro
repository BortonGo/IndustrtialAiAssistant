# Alternative qmake project. CMake is the primary build described in README.
QT += core gui widgets network
CONFIG += c++1z
TARGET = IndustrialAiAssistant
TEMPLATE = app

INCLUDEPATH += include include/application include/chat include/documents include/io include/llm include/mcp include/rag include/ui

SOURCES += src/application/assistant_service.cpp
SOURCES += src/application/mainwindow.cpp
SOURCES += src/chat/chat_list_model.cpp
SOURCES += src/chat/chat_manager.cpp
SOURCES += src/documents/document_list_model.cpp
SOURCES += src/io/document_loader.cpp
SOURCES += src/io/document_manager.cpp
SOURCES += src/io/idocument_reader.cpp
SOURCES += src/io/pdf_text_extractor.cpp
SOURCES += src/llm/illm_client.cpp
SOURCES += src/llm/lmstudio_llm_client.cpp
SOURCES += src/main.cpp
SOURCES += src/mcp/mcp_client.cpp
SOURCES += src/rag/document_chunker.cpp
SOURCES += src/rag/embedding_client.cpp
SOURCES += src/rag/vector_store.cpp
SOURCES += src/ui/chat_widget.cpp
SOURCES += src/ui/documents_widget.cpp
SOURCES += src/ui/message_composer.cpp
SOURCES += src/ui/message_widget.cpp
SOURCES += src/ui/sidebar_widget.cpp

HEADERS += include/application/assistant_service.hpp
HEADERS += include/application/mainwindow.h
HEADERS += include/chat/chat_list_model.hpp
HEADERS += include/chat/chat_manager.hpp
HEADERS += include/chat/chat.hpp
HEADERS += include/documents/document_list_model.hpp
HEADERS += include/documents/document.hpp
HEADERS += include/io/document_loader.hpp
HEADERS += include/io/document_manager.hpp
HEADERS += include/io/idocument_reader.hpp
HEADERS += include/io/pdf_text_extractor.hpp
HEADERS += include/llm/illm_client.hpp
HEADERS += include/llm/lmstudio_llm_client.hpp
HEADERS += include/mcp/mcp_client.hpp
HEADERS += include/rag/chunk.h
HEADERS += include/rag/document_chunker.h
HEADERS += include/rag/embedding_client.hpp
HEADERS += include/rag/vector_store.hpp
HEADERS += include/ui/chat_widget.hpp
HEADERS += include/ui/documents_widget.hpp
HEADERS += include/ui/message_composer.hpp
HEADERS += include/ui/message_widget.hpp
HEADERS += include/ui/sidebar_widget.hpp

FORMS += src/ui/mainwindow.ui
RESOURCES += resources/resources.qrc

win32 {
    LIBS += -ldwmapi
    RC_FILE = resources/app.rc
    RC_INCLUDEPATH += $$PWD/resources
}
