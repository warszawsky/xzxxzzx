#pragma once

#include "Config.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QRegularExpression>

#include <functional>

// Тонкий клиент REST API Home Assistant (локальная сеть).
// Модель видит и трогает только устройства из homeassistant/allowedEntities,
// домен сервиса всегда берётся из entity_id, поэтому вызвать shell_command, script.*
// и прочее «не устройство» через питомца нельзя, если ты сам их не разрешил.
class HomeAssistant : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(const QJsonObject&)>;

    HomeAssistant(const Config& cfg, QObject* parent = nullptr);

    bool isConfigured() const;
    bool isAllowed(const QString& entityId) const;
    bool needsConfirmation(const QString& entityId) const;

    void listEntities(const Done& done);
    void state(const QString& entityId, const Done& done);
    void call(const QString& entityId, const QString& service, const QJsonObject& data, const Done& done);

private:
    using RawDone = std::function<void(int status, const QByteArray& body, const QString& netError)>;
    void request(const QByteArray& verb, const QString& path, const QJsonObject& body, const RawDone& done);

    QUrl m_base;
    QString m_token;
    QList<QRegularExpression> m_allowed;
    QStringList m_confirmDomains;
    QNetworkAccessManager m_nam;   // свой, без прокси: HA живёт в локальной сети
};
