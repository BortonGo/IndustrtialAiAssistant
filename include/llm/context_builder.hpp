#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <functional>
#include <vector>
#include "model_api_config.hpp"

class QNetworkAccessManager;

struct ContextSource {
    QString text;
    QString imagePath;
    // Selection sees a compact description; generation gets the complete source.
    QString selectionText;
    QString groupId; // A table/section and its images are kept or removed together.
    bool required = false;
    bool containsTables = false;
    QJsonObject location; // Extractor provenance, never model-generated links.
};
struct ConversationTurn { QString question; QString answer; };
enum class SystemStatusAccess { Unresolved, NotRequested, Requested };
struct GenerationRequest {
    QString question;
    QStringList attachedFiles; // Explicit attachments of this question, not the chat library.
    QString systemStatus;
    std::vector<ContextSource> sources;
    std::vector<ConversationTurn> history;
    int contextSize = 8192;
    int maxAnswerTokens = 1500;
    SystemStatusAccess statusAccess = SystemStatusAccess::Unresolved;
};
struct PreparedContext {
    QJsonArray messages;
    QString sourcesSnapshot;
    QJsonArray sourceReferences;
    int textTokens = 0;
    int imageReserve = 0;
    int historyPairs = 0;
    int sourceCount = 0;
    int contextSize = 0;
    int maxAnswerTokens = 0;
    bool systemStatusRequested = false;
};

// Counts the actual model's chat template and tokenizer, then selects a suffix
// of complete conversation turns. Images have a conservative separate reserve.
class ContextBuilder final : public QObject {
    Q_OBJECT
public:
    enum class Purpose { Answer, Selection };
    using Callback = std::function<void(const PreparedContext&, const QString&)>;
    explicit ContextBuilder(const ModelApiConfig& config, QObject* parent = nullptr);
    void build(const GenerationRequest& request, Callback callback, Purpose purpose = Purpose::Answer);
    void cancel();
    static constexpr int safetyTokens = 128;
    static constexpr int imageTokenReserve = 1056; // server --image-max-tokens 1024 + delimiters
private:
    ModelApiConfig config_;
    QNetworkAccessManager* manager_;
    GenerationRequest request_;
    Purpose purpose_ = Purpose::Answer;
    Callback callback_;
    QMap<QString, QString> imageUrls_;
    PreparedContext best_;
    int sourceCount_ = 0, low_ = 0, high_ = 0;
    quint64 version_ = 0;
    PreparedContext candidate(int pairs) const;
    void measure(int pairs, bool base);
    void search();
    void finish(const QString& error = {});
    void post(const QString& endpoint, const QJsonObject& body,
              std::function<void(const QJsonObject&, const QString&)> callback);
};
