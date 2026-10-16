#include "LocalBridge.h"

#include <QJsonDocument>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr qsizetype kMaxHeader = 8 * 1024;
constexpr qsizetype kMaxBody = 64 * 1024;
constexpr int kMaxConnections = 32;
constexpr int kHeaderTimeoutMs = 10'000;
constexpr int kConnectionTimeoutMs = 120'000;

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

QByteArray reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 502: return "Bad Gateway";
    default: return "Error";
    }
}

} // namespace

LocalBridge::LocalBridge(const Config& cfg, QObject* parent) : QObject(parent), m_cfg(cfg)
{
    for (const QString& s : cfg.bridgeAllowedSubnets) {
        const auto subnet = QHostAddress::parseSubnet(s);
        if (subnet.second >= 0)
            m_subnets << subnet;
    }
    connect(&m_server, &QTcpServer::newConnection, this, &LocalBridge::onNewConnection);
}

void LocalBridge::addRoute(const QByteArray& method, const QByteArray& path, Handler handler)
{
    m_routes.insert(method + ' ' + path, std::move(handler));
}

bool LocalBridge::start(QString* error)
{
    QHostAddress host;
    if (m_cfg.bridgeHost == QLatin1String("0.0.0.0"))
        host = QHostAddress::AnyIPv4;
    else if (!host.setAddress(m_cfg.bridgeHost)) {
        *error = tr("bridge/host: некорректный адрес %1").arg(m_cfg.bridgeHost);
        return false;
    }
    if (!m_server.listen(host, m_cfg.bridgePort)) {
        *error = tr("Мост не стартовал на %1:%2: %3").arg(m_cfg.bridgeHost).arg(m_cfg.bridgePort).arg(m_server.errorString());
        return false;
    }
    return true;
}

QString LocalBridge::address() const
{
    return QStringLiteral("http://%1:%2").arg(m_cfg.bridgeHost).arg(m_server.serverPort());
}

bool LocalBridge::peerAllowed(QHostAddress address) const
{
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);   // ::ffff:192.168.1.5 -> 192.168.1.5
    if (isV4)
        address = QHostAddress(v4);
    if (address.isLoopback())
        return true;
    for (const auto& subnet : m_subnets)
        if (address.isInSubnet(subnet))
            return true;
    return false;
}

void LocalBridge::onNewConnection()
{
    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        if (m_connections.size() >= kMaxConnections || !peerAllowed(socket->peerAddress())) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_connections.insert(socket, Connection{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_connections.remove(socket);
            socket->deleteLater();
        });
        QTimer::singleShot(kHeaderTimeoutMs, socket, [this, socket] {
            if (!m_connections.value(socket).handled)
                socket->abort();
        });
        QTimer::singleShot(kConnectionTimeoutMs, socket, [socket] { socket->abort(); });
    }
}

void LocalBridge::onReadyRead(QTcpSocket* socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->handled)
        return;
    it->buffer += socket->readAll();

    const qsizetype headerEnd = it->buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (it->buffer.size() > kMaxHeader) {
            it->handled = true;
            reply(socket, 431, {{QStringLiteral("error"), QStringLiteral("headers too large")}});
        }
        return;
    }

    const QList<QByteArray> lines = it->buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    if (requestLine.size() != 3) {
        it->handled = true;
        reply(socket, 400, {{QStringLiteral("error"), QStringLiteral("bad request line")}});
        return;
    }
    QHash<QByteArray, QByteArray> headers;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const qsizetype colon = lines.at(i).indexOf(':');
        if (colon > 0)
            headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
    }

    bool ok = true;
    const qsizetype length = headers.value("content-length", "0").toLongLong(&ok);
    if (!ok || length < 0 || length > kMaxBody) {
        it->handled = true;
        reply(socket, 413, {{QStringLiteral("error"), QStringLiteral("body too large")}});
        return;
    }
    if (it->buffer.size() < headerEnd + 4 + length)
        return;   // тело ещё не дочитали

    it->handled = true;
    const QByteArray body = it->buffer.mid(headerEnd + 4, length);
    const QByteArray target = requestLine.at(1);
    const qsizetype query = target.indexOf('?');
    const QByteArray path = query < 0 ? target : target.left(query);
    dispatch(socket, requestLine.at(0).toUpper(), path, headers, body);
}

void LocalBridge::dispatch(QTcpSocket* socket, const QByteArray& method, const QByteArray& path,
                           const QHash<QByteArray, QByteArray>& headers, const QByteArray& body)
{
    const QByteArray auth = headers.value("authorization");
    if (!auth.startsWith("Bearer ") || !constantTimeEquals(auth.mid(7).trimmed(), m_cfg.bridgeToken.toUtf8())) {
        reply(socket, 401, {{QStringLiteral("error"), QStringLiteral("bad token")}});
        return;
    }

    const auto route = m_routes.constFind(method + ' ' + path);
    if (route == m_routes.cend()) {
        bool pathExists = false;
        for (auto r = m_routes.cbegin(); r != m_routes.cend(); ++r)
            pathExists |= r.key().endsWith(' ' + path);
        reply(socket, pathExists ? 405 : 404, {{QStringLiteral("error"), QStringLiteral("no such route")}});
        return;
    }

    QJsonObject request;
    if (!body.isEmpty()) {
        if (!headers.value("content-type").startsWith("application/json")) {
            reply(socket, 415, {{QStringLiteral("error"), QStringLiteral("use application/json")}});
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            reply(socket, 400, {{QStringLiteral("error"), QStringLiteral("body must be a JSON object")}});
            return;
        }
        request = doc.object();
    }

    // Обработчик может ответить позже (например, после ответа Gemini). Сокет к тому времени
    // мог закрыться, поэтому держим QPointer.
    QPointer<QTcpSocket> guard(socket);
    route.value()(request, [guard](int status, const QJsonObject& response) {
        if (guard)
            reply(guard, status, response);
    });
}

void LocalBridge::reply(QTcpSocket* socket, int status, const QJsonObject& body)
{
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: " + QByteArray::number(payload.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + payload;
    socket->write(out);
    socket->disconnectFromHost();
}
