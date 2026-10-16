#include "HomeAssistant.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>

namespace {

const QRegularExpression kEntityRe(QStringLiteral("^[a-z_]+\\.[a-z0-9_]+$"));
const QRegularExpression kServiceRe(QStringLiteral("^[a-z_]+$"));

QJsonObject error(const QString& message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

QJsonObject compactState(const QJsonObject& s)
{
    const QJsonObject attrs = s.value(QStringLiteral("attributes")).toObject();
    QJsonObject out{{QStringLiteral("entity_id"), s.value(QStringLiteral("entity_id"))},
                    {QStringLiteral("name"), attrs.value(QStringLiteral("friendly_name"))},
                    {QStringLiteral("state"), s.value(QStringLiteral("state"))}};
    // Пара полезных атрибутов, без простыни из десятков полей.
    for (const char* key : {"unit_of_measurement", "brightness", "current_temperature", "temperature"})
        if (attrs.contains(QLatin1String(key)))
            out[QLatin1String(key)] = attrs.value(QLatin1String(key));
    return out;
}

} // namespace

HomeAssistant::HomeAssistant(const Config& cfg, QObject* parent)
    : QObject(parent), m_base(cfg.haUrl), m_token(cfg.haToken), m_confirmDomains(cfg.haConfirmDomains)
{
    m_nam.setProxy(QNetworkProxy::NoProxy);
    for (const QString& pattern : cfg.haAllowedEntities)
        m_allowed << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern.trimmed()));
}

bool HomeAssistant::isConfigured() const
{
    return m_base.isValid() && !m_base.host().isEmpty() && !m_token.isEmpty() && !m_allowed.isEmpty();
}

bool HomeAssistant::isAllowed(const QString& entityId) const
{
    if (!kEntityRe.match(entityId).hasMatch())
        return false;
    for (const QRegularExpression& re : m_allowed)
        if (re.match(entityId).hasMatch())
            return true;
    return false;
}

bool HomeAssistant::needsConfirmation(const QString& entityId) const
{
    return m_confirmDomains.contains(entityId.section(QLatin1Char('.'), 0, 0));
}

void HomeAssistant::listEntities(const Done& done)
{
    request("GET", QStringLiteral("/api/states"), {}, [this, done](int status, const QByteArray& body, const QString& err) {
        if (status != 200)
            return done(error(QStringLiteral("HA ответил %1 %2").arg(status).arg(err)));
        QJsonArray out;
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            const QJsonObject s = v.toObject();
            if (isAllowed(s.value(QStringLiteral("entity_id")).toString()))
                out << compactState(s);
        }
        done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("devices"), out}});
    });
}

void HomeAssistant::state(const QString& entityId, const Done& done)
{
    if (!isAllowed(entityId))
        return done(error(QStringLiteral("устройство не в белом списке")));
    request("GET", QStringLiteral("/api/states/") + entityId, {},
            [done](int status, const QByteArray& body, const QString& err) {
                if (status != 200)
                    return done(error(QStringLiteral("HA ответил %1 %2").arg(status).arg(err)));
                QJsonObject out = compactState(QJsonDocument::fromJson(body).object());
                out[QStringLiteral("ok")] = true;
                done(out);
            });
}

void HomeAssistant::call(const QString& entityId, const QString& service, const QJsonObject& data, const Done& done)
{
    if (!isAllowed(entityId))
        return done(error(QStringLiteral("устройство не в белом списке")));
    if (!kServiceRe.match(service).hasMatch())
        return done(error(QStringLiteral("некорректное имя сервиса")));

    const QString domain = entityId.section(QLatin1Char('.'), 0, 0);
    QJsonObject body = data;
    body[QStringLiteral("entity_id")] = entityId;   // перекрывает entity_id из data_json

    request("POST", QStringLiteral("/api/services/%1/%2").arg(domain, service), body,
            [this, entityId, done](int status, const QByteArray&, const QString& err) {
                if (status != 200)
                    return done(error(QStringLiteral("HA ответил %1 %2").arg(status).arg(err)));
                // Сервис отработал. Возвращаем свежее состояние, чтобы модель не гадала.
                state(entityId, done);
            });
}

void HomeAssistant::request(const QByteArray& verb, const QString& path, const QJsonObject& body, const RawDone& done)
{
    QUrl url = m_base;
    url.setPath(url.path().chopped(url.path().endsWith(QLatin1Char('/')) ? 1 : 0) + path);
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(10'000);

    QNetworkReply* reply = verb == "GET"
        ? m_nam.get(req)
        : m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll(),
             reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
    });
}
