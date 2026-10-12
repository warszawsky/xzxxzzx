#pragma once

#include "Config.h"

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>

#include <functional>

class QTcpSocket;

// Минимальный HTTP/1.1 сервер с JSON-API, чтобы питомцем могли пользоваться другие локальные
// сервисы: Home Assistant шлёт уведомления, скрипты спрашивают модель и т.д.
//
// Безопасность по умолчанию:
//  * слушает только 127.0.0.1; для локальной сети надо явно поставить bridge/host=0.0.0.0;
//  * каждый запрос с заголовком Authorization: Bearer <bridge/token>;
//  * чужие подсети отсекаются до разбора запроса (bridge/allowedSubnets);
//  * только application/json и тела до 64 КБ; CORS-заголовков нет, поэтому сайт в браузере
//    не сможет дёрнуть API, даже если узнает порт.
class LocalBridge : public QObject
{
    Q_OBJECT
public:
    using Respond = std::function<void(int status, const QJsonObject& body)>;
    using Handler = std::function<void(const QJsonObject& request, const Respond& respond)>;

    explicit LocalBridge(const Config& cfg, QObject* parent = nullptr);

    void addRoute(const QByteArray& method, const QByteArray& path, Handler handler);
    bool start(QString* error);
    QString address() const;

private:
    struct Connection
    {
        QByteArray buffer;
        bool handled = false;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void dispatch(QTcpSocket* socket, const QByteArray& method, const QByteArray& path,
                  const QHash<QByteArray, QByteArray>& headers, const QByteArray& body);
    bool peerAllowed(QHostAddress address) const;
    static void reply(QTcpSocket* socket, int status, const QJsonObject& body);

    Config m_cfg;
    QTcpServer m_server;
    QHash<QTcpSocket*, Connection> m_connections;
    QHash<QByteArray, Handler> m_routes;   // "POST /api/notify" -> handler
    QList<QPair<QHostAddress, int>> m_subnets;
};
