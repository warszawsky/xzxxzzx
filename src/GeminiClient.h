#pragma once

#include "Config.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

#include <functional>

class LocalTools;
class QNetworkReply;

struct Attachment
{
    QByteArray data;   // сырые байты (PNG/JPEG), в base64 кодируются при отправке
    QString mimeType;
};

// Клиент Gemini API (REST, v1beta) без SDK.
//
// Что делает:
//  * держит историю диалога в памяти (на диск ничего не пишется);
//  * стримит ответ через streamGenerateContent?alt=sse, текст приходит кусками;
//  * крутит цикл function calling: модель просит вызвать функцию -> LocalTools выполняет ->
//    результат уходит обратно -> модель отвечает текстом (до kMaxToolRounds раундов);
//  * части ответа модели (включая thoughtSignature у Gemini 3) кладутся в историю как есть,
//    иначе API отклонит следующий запрос с function calling.
class GeminiClient : public QObject
{
    Q_OBJECT
public:
    GeminiClient(const Config& cfg, LocalTools* tools, QObject* parent = nullptr);

    void setApiKey(const QString& key) { m_cfg.apiKey = key; }
    bool hasApiKey() const { return !m_cfg.apiKey.isEmpty(); }
    bool isBusy() const { return m_active; }

    // Сообщение в основной диалог. webSearch = ответ с поиском Google вместо локальных функций.
    // false (и никаких сигналов), если уже идёт ответ или нет ключа: проверяй isBusy()/hasApiKey().
    bool send(const QString& text, const Attachment& image = {}, bool webSearch = false);
    void cancel();
    void resetConversation();

    // Заранее открывает TLS-соединение, чтобы первый запрос не ждал рукопожатия.
    void warmUp();

    // Разовый запрос без истории и без функций. Нужен локальному мосту: внешние сервисы
    // могут спросить модель, но не могут через неё дёргать таймеры или умный дом.
    using AskCallback = std::function<void(const QString& reply, const QString& error)>;
    void ask(const QString& text, AskCallback cb);

signals:
    void replyStarted();
    void textDelta(const QString& delta);
    void toolInvoked(const QString& name, const QJsonObject& args);
    void replyFinished(const QString& fullText, const QStringList& sources);
    void replyFailed(const QString& message);

private:
    QNetworkRequest makeRequest(bool stream) const;
    QJsonObject buildBody(const QJsonArray& contents, bool withTools, bool webSearch) const;
    QString systemPrompt() const;

    void startRound();
    void onReadyRead();
    void onFinished();
    void drainSse(bool flush);
    void handleChunk(const QByteArray& json);
    void runToolCalls(const QJsonArray& calls);
    void fail(const QString& message);
    void rollbackTurn();
    void trimHistory();
    void dropOldImages();

    static void mergePart(QJsonArray& parts, const QJsonObject& part);
    static QString describeError(int httpStatus, const QByteArray& body, const QString& netError,
                                 const QString& model);

    Config m_cfg;
    LocalTools* m_tools;
    QNetworkAccessManager m_nam;

    QJsonArray m_history;
    int m_turnStart = 0;      // индекс в истории, с которого начался текущий ход пользователя
    int m_toolRounds = 0;
    int m_retries = 0;
    int m_generation = 0;     // растёт при отмене: поздние колбэки от функций игнорируются
    bool m_active = false;
    bool m_webSearch = false;

    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer;
    QJsonArray m_modelParts;  // части ответа модели в текущем раунде
    QString m_finishReason;
    QString m_streamError;
    QString m_turnText;       // весь видимый текст хода (через все раунды)
    QStringList m_sources;
};
