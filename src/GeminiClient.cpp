#include "GeminiClient.h"

#include "LocalTools.h"
#include "Persona.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QScreen>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>

#include <memory>

namespace {

constexpr int kMaxToolRounds = 5;
constexpr int kTransferTimeoutMs = 90'000;

QString timeStamp()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int off = now.offsetFromUtc();
    const QString tz = QStringLiteral("UTC%1%2:%3")
                           .arg(off >= 0 ? QLatin1Char('+') : QLatin1Char('-'))
                           .arg(qAbs(off) / 3600, 2, 10, QLatin1Char('0'))
                           .arg((qAbs(off) % 3600) / 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("[время: %1, %2, %3]")
        .arg(now.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             QLocale(QLocale::Russian).dayName(now.date().dayOfWeek()), tz);
}

QJsonObject textPart(const QString& text)
{
    return QJsonObject{{QStringLiteral("text"), text}};
}

bool isUserTextTurn(const QJsonObject& content)
{
    if (content.value(QStringLiteral("role")).toString() != QLatin1String("user"))
        return false;
    for (const QJsonValue& p : content.value(QStringLiteral("parts")).toArray())
        if (p.toObject().contains(QStringLiteral("functionResponse")))
            return false;
    return true;
}

} // namespace

GeminiClient::GeminiClient(const Config& cfg, LocalTools* tools, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_tools(tools)
{
    if (m_cfg.model.startsWith(QLatin1String("models/")))
        m_cfg.model = m_cfg.model.mid(7);
    while (m_cfg.baseUrl.endsWith(QLatin1Char('/')))
        m_cfg.baseUrl.chop(1);

    if (!m_cfg.proxy.isEmpty()) {
        const QUrl u(m_cfg.proxy);
        const auto type = u.scheme().startsWith(QLatin1String("socks")) ? QNetworkProxy::Socks5Proxy
                                                                         : QNetworkProxy::HttpProxy;
        m_nam.setProxy(QNetworkProxy(type, u.host(), quint16(u.port(1080)), u.userName(), u.password()));
    }
    // Без явного прокси используется системный (QNetworkProxyFactory::setUseSystemConfiguration в main).
}

// ---------------------------------------------------------------------------------------------
// Построение запроса

QNetworkRequest GeminiClient::makeRequest(bool stream) const
{
    QUrl url(QStringLiteral("%1/models/%2:%3")
                 .arg(m_cfg.baseUrl, m_cfg.model,
                      stream ? QStringLiteral("streamGenerateContent") : QStringLiteral("generateContent")));
    if (stream)
        url.setQuery(QStringLiteral("alt=sse"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Ключ в заголовке, а не в ?key=..., чтобы он не попадал в логи прокси и URL.
    req.setRawHeader("x-goog-api-key", m_cfg.apiKey.toUtf8());
    req.setTransferTimeout(kTransferTimeoutMs);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    return req;
}

QString GeminiClient::systemPrompt() const
{
    // Здесь только то, что не меняется между запросами. Тогда префикс запроса
    // (system instruction + старая история) одинаковый, и Gemini может кэшировать его
    // (implicit caching): быстрее и дешевле. Время уходит меткой в каждое сообщение.
    QString ctx = QString::fromUtf8(kPersona);
    ctx += QStringLiteral("\n[контекст]\n- ОС: %1\n").arg(QSysInfo::prettyProductName());
    if (const QScreen* s = QGuiApplication::primaryScreen()) {
        const QSize px = s->geometry().size() * s->devicePixelRatio();
        ctx += QStringLiteral("- Основной экран: %1×%2\n").arg(px.width()).arg(px.height());
    }
    ctx += QStringLiteral("- Умный дом: %1\n")
               .arg(m_tools && m_tools->hasSmartHome() ? QStringLiteral("подключён (Home Assistant)")
                                                       : QStringLiteral("не подключён"));
    return ctx;
}

QJsonObject GeminiClient::buildBody(const QJsonArray& contents, bool withTools, bool webSearch) const
{
    QJsonObject body;
    body[QStringLiteral("systemInstruction")] =
        QJsonObject{{QStringLiteral("parts"), QJsonArray{textPart(systemPrompt())}}};
    body[QStringLiteral("contents")] = contents;

    QJsonArray tools;
    if (webSearch) {
        tools.append(QJsonObject{{QStringLiteral("googleSearch"), QJsonObject{}}});
    } else if (withTools && m_tools) {
        const QJsonArray decls = m_tools->declarations();
        if (!decls.isEmpty())
            tools.append(QJsonObject{{QStringLiteral("functionDeclarations"), decls}});
    }
    if (!tools.isEmpty())
        body[QStringLiteral("tools")] = tools;

    QJsonObject gen{{QStringLiteral("maxOutputTokens"), m_cfg.maxOutputTokens}};
    if (!m_cfg.thinkingLevel.isEmpty())
        gen[QStringLiteral("thinkingConfig")] =
            QJsonObject{{QStringLiteral("thinkingLevel"), m_cfg.thinkingLevel}};
    body[QStringLiteral("generationConfig")] = gen;
    return body;
}

// ---------------------------------------------------------------------------------------------
// Публичные методы

void GeminiClient::warmUp()
{
    const QUrl u(m_cfg.baseUrl);
    if (u.scheme() == QLatin1String("https"))
        m_nam.connectToHostEncrypted(u.host(), quint16(u.port(443)));
}

bool GeminiClient::send(const QString& text, const Attachment& image, bool webSearch)
{
    if (m_active || m_cfg.apiKey.isEmpty())
        return false;

    if (!image.data.isEmpty())
        dropOldImages();

    m_turnStart = m_history.size();
    QJsonArray parts;
    if (!image.data.isEmpty()) {
        // Картинка инлайном в запросе (base64), без Files API: в облаке Google не остаётся
        // загруженного файла, а локально файла не было вообще.
        parts.append(QJsonObject{
            {QStringLiteral("inlineData"),
             QJsonObject{{QStringLiteral("mimeType"), image.mimeType},
                         {QStringLiteral("data"), QString::fromLatin1(image.data.toBase64())}}}});
    }
    parts.append(textPart(timeStamp() + QLatin1Char('\n') + text));
    m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                 {QStringLiteral("parts"), parts}});

    m_active = true;
    m_webSearch = webSearch;
    m_toolRounds = 0;
    m_retries = 0;
    m_turnText.clear();
    m_sources.clear();
    emit replyStarted();
    startRound();
    return true;
}

void GeminiClient::cancel()
{
    if (!m_active)
        return;
    ++m_generation;
    m_active = false;
    if (QNetworkReply* r = m_reply.data()) {
        m_reply.clear();
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    rollbackTurn();
}

void GeminiClient::resetConversation()
{
    cancel();
    m_history = QJsonArray();
}

void GeminiClient::ask(const QString& text, AskCallback cb)
{
    if (m_cfg.apiKey.isEmpty()) {
        cb({}, tr("Нет API-ключа"));
        return;
    }
    const QJsonArray contents{QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("parts"), QJsonArray{textPart(timeStamp() + QLatin1Char('\n') + text)}}}};
    const QByteArray body = QJsonDocument(buildBody(contents, false, false)).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_nam.post(makeRequest(false), body);
    const QString model = m_cfg.model;
    connect(reply, &QNetworkReply::finished, this, [reply, cb, model] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        if (status != 200) {
            cb({}, describeError(status, data, reply->errorString(), model));
            return;
        }
        QString out;
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        const QJsonObject cand = obj.value(QStringLiteral("candidates")).toArray().first().toObject();
        for (const QJsonValue& p : cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray())
            if (!p.toObject().value(QStringLiteral("thought")).toBool())
                out += p.toObject().value(QStringLiteral("text")).toString();
        if (out.isEmpty())
            cb({}, tr("Пустой ответ модели (%1)").arg(cand.value(QStringLiteral("finishReason")).toString()));
        else
            cb(out, {});
    });
}

// ---------------------------------------------------------------------------------------------
// Один раунд стриминга

void GeminiClient::startRound()
{
    m_buffer.clear();
    m_modelParts = QJsonArray();
    m_finishReason.clear();
    m_streamError.clear();

    const QByteArray body = QJsonDocument(buildBody(m_history, true, m_webSearch)).toJson(QJsonDocument::Compact);
    m_reply = m_nam.post(makeRequest(true), body);
    connect(m_reply, &QNetworkReply::readyRead, this, &GeminiClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &GeminiClient::onFinished);
}

void GeminiClient::onReadyRead()
{
    if (!m_reply)
        return;
    m_buffer += m_reply->readAll();
    // Тело ошибки приходит обычным JSON, не SSE: разберём его целиком в onFinished.
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
        drainSse(false);
}

void GeminiClient::drainSse(bool flush)
{
    m_buffer.replace("\r\n", "\n");
    for (;;) {
        qsizetype end = m_buffer.indexOf("\n\n");
        if (end < 0) {
            if (!flush || m_buffer.trimmed().isEmpty())
                return;
            end = m_buffer.size();
        }
        const QByteArray event = m_buffer.left(end);
        m_buffer.remove(0, qMin(end + 2, m_buffer.size()));

        QByteArray data;
        for (const QByteArray& line : event.split('\n')) {
            if (line.startsWith("data:")) {
                if (!data.isEmpty())
                    data += '\n';
                data += line.mid(5).trimmed();
            }
        }
        if (!data.isEmpty())
            handleChunk(data);
    }
}

void GeminiClient::handleChunk(const QByteArray& json)
{
    const QJsonObject obj = QJsonDocument::fromJson(json).object();
    if (obj.isEmpty())
        return;

    if (obj.contains(QStringLiteral("error"))) {
        m_streamError = obj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        return;
    }
    const QString block = obj.value(QStringLiteral("promptFeedback")).toObject().value(QStringLiteral("blockReason")).toString();
    if (!block.isEmpty()) {
        m_streamError = tr("Gemini заблокировал запрос фильтром (%1)").arg(block);
        return;
    }

    const QJsonObject cand = obj.value(QStringLiteral("candidates")).toArray().first().toObject();
    const QJsonArray parts = cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray();
    for (const QJsonValue& v : parts) {
        const QJsonObject part = v.toObject();
        if (part.value(QStringLiteral("thought")).toBool())
            continue;   // «мысли» модели не показываем и не храним
        mergePart(m_modelParts, part);
        const QString text = part.value(QStringLiteral("text")).toString();
        if (!text.isEmpty()) {
            m_turnText += text;
            emit textDelta(text);
        }
    }

    const QString reason = cand.value(QStringLiteral("finishReason")).toString();
    if (!reason.isEmpty())
        m_finishReason = reason;

    // Источники при ответе с поиском Google.
    const QJsonArray chunks = cand.value(QStringLiteral("groundingMetadata")).toObject()
                                  .value(QStringLiteral("groundingChunks")).toArray();
    for (const QJsonValue& c : chunks) {
        const QJsonObject web = c.toObject().value(QStringLiteral("web")).toObject();
        const QString uri = web.value(QStringLiteral("uri")).toString();
        if (uri.isEmpty())
            continue;
        const QString title = web.value(QStringLiteral("title")).toString(uri);
        const QString link = QStringLiteral("[%1](%2)").arg(title, uri);
        if (!m_sources.contains(link))
            m_sources << link;
    }
}

// Склеивает стримовые куски в одну часть, но не трогает functionCall и не теряет thoughtSignature:
// у Gemini 3 подпись может прийти отдельным куском с пустым текстом, и её надо вернуть в истории.
void GeminiClient::mergePart(QJsonArray& parts, const QJsonObject& part)
{
    const auto isPlainText = [](const QJsonObject& p) {
        return p.contains(QStringLiteral("text")) && !p.contains(QStringLiteral("functionCall"));
    };
    if (isPlainText(part) && !parts.isEmpty()) {
        QJsonObject last = parts.last().toObject();
        const bool bothSigned = last.contains(QStringLiteral("thoughtSignature"))
                                && part.contains(QStringLiteral("thoughtSignature"));
        if (isPlainText(last) && !bothSigned) {
            last[QStringLiteral("text")] = last.value(QStringLiteral("text")).toString()
                                           + part.value(QStringLiteral("text")).toString();
            if (part.contains(QStringLiteral("thoughtSignature")))
                last[QStringLiteral("thoughtSignature")] = part.value(QStringLiteral("thoughtSignature"));
            parts[parts.size() - 1] = last;
            return;
        }
    }
    parts.append(part);
}

void GeminiClient::onFinished()
{
    QNetworkReply* reply = m_reply.data();
    if (!reply)
        return;
    m_reply.clear();
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_buffer += reply->readAll();

    if (status != 200 || reply->error() != QNetworkReply::NoError) {
        // 500/503 у Gemini часто означают «модель перегружена». Один повтор, если ещё ничего не показали.
        if ((status == 500 || status == 503) && m_retries == 0 && m_modelParts.isEmpty()) {
            ++m_retries;
            const int gen = m_generation;
            QTimer::singleShot(1500, this, [this, gen] {
                if (gen == m_generation && m_active)
                    startRound();
            });
            return;
        }
        fail(describeError(status, m_buffer, reply->errorString(), m_cfg.model));
        return;
    }

    drainSse(true);

    if (!m_streamError.isEmpty()) {
        fail(m_streamError);
        return;
    }
    if (m_modelParts.isEmpty()) {
        fail(m_finishReason.isEmpty() ? tr("Пустой ответ модели")
                                      : tr("Модель не ответила (finishReason: %1)").arg(m_finishReason));
        return;
    }

    m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("model")},
                                 {QStringLiteral("parts"), m_modelParts}});

    QJsonArray calls;
    for (const QJsonValue& p : std::as_const(m_modelParts))
        if (p.toObject().contains(QStringLiteral("functionCall")))
            calls.append(p.toObject().value(QStringLiteral("functionCall")));

    if (!calls.isEmpty()) {
        if (++m_toolRounds > kMaxToolRounds) {
            fail(tr("Модель зациклилась на вызовах функций, остановил."));
            return;
        }
        runToolCalls(calls);
        return;
    }

    if (m_finishReason == QLatin1String("MAX_TOKENS")) {
        const QString note = tr("\n\n_…ответ обрезан по лимиту токенов_");
        m_turnText += note;
        emit textDelta(note);
    }

    m_active = false;
    trimHistory();
    emit replyFinished(m_turnText, m_sources);
}

// ---------------------------------------------------------------------------------------------
// Function calling

void GeminiClient::runToolCalls(const QJsonArray& calls)
{
    struct Pending
    {
        QJsonArray responses;
        int remaining = 0;
    };
    auto pending = std::make_shared<Pending>();
    pending->remaining = int(calls.size());
    for (int i = 0; i < calls.size(); ++i)
        pending->responses.append(QJsonValue());

    const int gen = m_generation;
    for (int i = 0; i < calls.size(); ++i) {
        const QJsonObject call = calls.at(i).toObject();
        const QString name = call.value(QStringLiteral("name")).toString();
        const QJsonObject args = call.value(QStringLiteral("args")).toObject();
        emit toolInvoked(name, args);

        auto done = [this, pending, gen, i, call](const QJsonObject& result) {
            if (gen != m_generation || !m_active)
                return;   // ход отменили, пока функция работала
            QJsonObject fr{{QStringLiteral("name"), call.value(QStringLiteral("name"))},
                           {QStringLiteral("response"), result}};
            if (call.contains(QStringLiteral("id")))
                fr[QStringLiteral("id")] = call.value(QStringLiteral("id"));
            pending->responses[i] = QJsonObject{{QStringLiteral("functionResponse"), fr}};
            if (--pending->remaining > 0)
                return;
            m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                         {QStringLiteral("parts"), pending->responses}});
            m_retries = 0;
            startRound();
        };

        if (m_tools)
            m_tools->invoke(name, args, done);
        else
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("tools unavailable")}});
    }
}

// ---------------------------------------------------------------------------------------------
// История

void GeminiClient::fail(const QString& message)
{
    m_active = false;
    ++m_generation;
    rollbackTurn();
    emit replyFailed(message);
}

// Убирает из истории недоделанный ход, иначе следующий запрос уйдёт с «висящим»
// сообщением пользователя или functionCall без ответа, и API его отклонит.
void GeminiClient::rollbackTurn()
{
    while (m_history.size() > m_turnStart)
        m_history.removeLast();
}

// Обрезает историю по границе реплики пользователя (не между functionCall и functionResponse).
void GeminiClient::trimHistory()
{
    QList<int> starts;
    for (int i = 0; i < m_history.size(); ++i)
        if (isUserTextTurn(m_history.at(i).toObject()))
            starts << i;
    const int excess = int(starts.size()) - m_cfg.maxHistoryTurns;
    if (excess <= 0)
        return;
    const int cut = starts.at(excess);
    for (int i = 0; i < cut; ++i)
        m_history.removeFirst();
}

// Старые скриншоты заменяются заглушкой: каждый стоит сотни токенов в каждом следующем запросе.
void GeminiClient::dropOldImages()
{
    for (int i = 0; i < m_history.size(); ++i) {
        QJsonObject content = m_history.at(i).toObject();
        QJsonArray parts = content.value(QStringLiteral("parts")).toArray();
        bool changed = false;
        for (int j = 0; j < parts.size(); ++j) {
            if (parts.at(j).toObject().contains(QStringLiteral("inlineData"))) {
                parts[j] = textPart(QStringLiteral("[здесь был старый скриншот, удалён из контекста]"));
                changed = true;
            }
        }
        if (changed) {
            content[QStringLiteral("parts")] = parts;
            m_history[i] = content;
        }
    }
}

// ---------------------------------------------------------------------------------------------

QString GeminiClient::describeError(int status, const QByteArray& body, const QString& netError,
                                    const QString& model)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject err = doc.isArray() ? doc.array().first().toObject().value(QStringLiteral("error")).toObject()
                                    : doc.object().value(QStringLiteral("error")).toObject();
    const QString msg = err.value(QStringLiteral("message")).toString();

    if (msg.contains(QLatin1String("location is not supported"), Qt::CaseInsensitive))
        return tr("Gemini API не работает из твоего региона (User location is not supported). "
                  "Пропиши прокси в network/proxy в конфиге.");
    if (msg.contains(QLatin1String("API key"), Qt::CaseInsensitive) || status == 401 || status == 403)
        return tr("Ключ не принят (%1). Проверь его в aistudio.google.com → API keys.").arg(msg.left(160));
    if (status == 0)
        return tr("Нет связи с Gemini: %1").arg(netError);
    if (status == 404)
        return tr("Модель «%1» не найдена. Поменяй gemini/model в конфиге.").arg(model);
    if (status == 429)
        return tr("Упёрся в лимит запросов (429). Подожди минуту или подключи биллинг в AI Studio.");
    if (status == 500 || status == 503)
        return tr("Серверы Gemini перегружены (%1). Попробуй ещё раз.").arg(status);
    if (status == 400 && msg.contains(QLatin1String("thinking"), Qt::CaseInsensitive))
        return tr("Модель не понимает thinkingLevel. Очисти gemini/thinkingLevel в конфиге. (%1)").arg(msg.left(160));
    return tr("Ошибка Gemini %1: %2").arg(status).arg(msg.isEmpty() ? netError : msg.left(300));
}
