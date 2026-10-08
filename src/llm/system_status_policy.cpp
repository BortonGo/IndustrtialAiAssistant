#include "system_status_policy.hpp"
#include <QRegularExpression>
#include <QSet>

namespace {
bool matches(const QString& text, const QString& expression) {
    return QRegularExpression(expression, QRegularExpression::CaseInsensitiveOption |
                              QRegularExpression::UseUnicodePropertiesOption).match(text).hasMatch();
}

const QString metric = QStringLiteral(
    "\\b(?:cpu|ram|цпу?|озу|процессор\\p{L}*|оперативн\\p{L}*|памят\\p{L}*)\\b");
const QString machine = QStringLiteral("\\b(?:систем\\p{L}*|компьютер\\p{L}*|комп|ноутбук\\p{L}*|пк|system|computer|pc)\\b");
const QRegularExpression measurement(QStringLiteral("(?<!\\d)\\d+(?:[.,]\\d+)?\\s*(?:%|процент\\p{L}*|percent|гб|мб|gib|mib|gb|mb)"),
    QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);

QSet<QString> measurements(const QString& text) {
    QSet<QString> values;
    auto matches = measurement.globalMatch(text);
    while (matches.hasNext()) {
        auto value = matches.next().captured().toLower();
        value.remove(QRegularExpression("\\s")); value.replace(',', '.');
        value.replace(QRegularExpression(QStringLiteral("процент\\p{L}*|percent"), QRegularExpression::UseUnicodePropertiesOption), "%");
        values.insert(value);
    }
    return values;
}

bool forbidden(const QString& question) {
    return matches(question, QStringLiteral(
        "(?:не|don't|do not)\\s+(?:надо\\s+|нужно\\s+)?(?:показыв|вывод|упомин|добав|сообщ|рассказыв|show|mention|include)[^.!?;]*") +
        "(?:" + metric + "|" + machine + ")");
}

bool directRequest(const QString& question) {
    if (forbidden(question)) return false;
    if (matches(question, QStringLiteral("(?:что такое|что означает|что значит|объясни термин|what (?:is|does).*mean)"))) return false;
    const bool now = matches(question, QStringLiteral("сейчас|текущ|мо[йёе]м?|этого компьютера|на этом|current|right now|my computer"));
    const bool document = matches(question, QStringLiteral("документ|инструкци|протокол|отч[её]т|табличк|manual|document|report"));
    if (document && !now) return false;
    const bool workload = matches(question, QStringLiteral(
        "загруз|нагруз|загруж|использ|занят|свобод|доступн|потребл|процент|статус|состояни|usage|load|utilization|used|free|status"));
    if (matches(question, metric) && (now || workload || matches(question, QStringLiteral("покажи|проверь|show|check")))) return true;
    return matches(question, machine) &&
            (workload || matches(question, QStringLiteral("как (?:сейчас |там |себя чувствует )")));
}

bool telemetry(const QString& sentence) {
    const bool measured = matches(sentence, QStringLiteral("\\d+(?:[.,]\\d+)?\\s*(?:%|процент|percent|гб|мб|gib|mib|gb|mb)"));
    if (matches(sentence, metric) && measured) return true;
    if (matches(sentence, QStringLiteral("get_system_status|(?:данны[емх]|показател\\p{L}*)[^.!?]*локальн\\p{L}* компьютер"))) return true;
    if (matches(sentence, machine) && matches(sentence, QStringLiteral("работает (?:стабильно|нормально|исправно)|running (?:normally|stable)"))) return true;
    // Standalone telemetry headings, not arbitrary technical discussion of CPU/RAM.
    const auto heading = QString(sentence).remove(QRegularExpression("[*_#>:]" )).trimmed();
    return matches(heading, QStringLiteral(
        "^(?:(?:текущее |текущая |текущие )?(?:состояние (?:системы|компьютера)|"
        "загрузка (?:cpu|процессора|цп)|использование (?:ram|озу|оперативной памяти)|"
        "показатели (?:системы|компьютера)|system status))$"));
}
}

bool requestsSystemStatus(const QString& question, const std::vector<ConversationTurn>& history) {
    if (forbidden(question)) return false;
    if (directRequest(question)) return true;
    // Resolve only a short explicit continuation of a user's monitoring question.
    // Assistant's unsolicited metrics never make later questions opt in.
    if (history.empty() || !matches(question.trimmed(), QStringLiteral(
            "^(?:(?:а|и|and)\\s+)?(?:сейчас|теперь|память|оперативная память|процессор|cpu|ram|озу|нагрузка|now)[?!. ]*$")))
        return false;
    return directRequest(history.back().question);
}

QString withoutSystemStatus(const QString& text, const QString& documentReferences) {
    QString normalized = text; normalized.replace("\r\n", "\n");
    QStringList lines;
    bool code = false, removed = false;
    const auto referenceValues = measurements(documentReferences);
    for (const auto& line : normalized.split('\n')) {
        if (line.trimmed().startsWith("```") || line.trimmed().startsWith("~~~")) {
            code = !code; lines.append(line); continue;
        }
        // Keep examples/programs intact; they are not prose telemetry reports.
        if (code) { lines.append(line); continue; }
        QStringList kept;
        for (const auto& sentence : line.split(QRegularExpression("(?<=[.!?;])\\s+"))) {
            const auto values = measurements(sentence);
            const bool documented = !values.isEmpty() && (values - referenceValues).isEmpty() &&
                    !matches(sentence, QStringLiteral("сейчас|текущ|локальн|last updated|right now"));
            if ((!documented && telemetry(sentence)) || (removed && matches(sentence, QStringLiteral(
                    "^\\s*[(>*_ ]*(?:последнее обновление|обновлено|время получения данных|last updated)")))) {
                removed = true;
                continue;
            }
            kept.append(sentence);
            if (!sentence.trimmed().isEmpty()) removed = false;
        }
        if (!kept.isEmpty()) lines.append(kept.join(" "));
    }
    QString result = lines.join("\n");
    result.replace(QRegularExpression("\n{3,}"), "\n\n");
    return result.trimmed();
}

GenerationRequest contextWithoutUnrequestedStatus(const GenerationRequest& request) {
    GenerationRequest result = request;
    if (result.statusAccess == SystemStatusAccess::Unresolved)
        result.statusAccess = requestsSystemStatus(request.question, request.history) ?
                    SystemStatusAccess::Requested : SystemStatusAccess::NotRequested;
    if (result.statusAccess == SystemStatusAccess::Requested) return result;
    result.systemStatus.clear();
    for (auto& turn : result.history) turn.answer = withoutSystemStatus(turn.answer);
    return result;
}
