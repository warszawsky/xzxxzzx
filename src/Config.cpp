#include "Config.h"

#include <QRandomGenerator>
#include <QSettings>

#include <memory>

namespace {

std::unique_ptr<QSettings> openSettings()
{
    return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                       QStringLiteral("Geminik"), QStringLiteral("geminik"));
}

// Читает значение, а если его нет, записывает дефолт в файл, чтобы конфиг был самодокументируемым.
QVariant readOrInit(QSettings& s, const QString& key, const QVariant& def)
{
    if (!s.contains(key)) {
        s.setValue(key, def);
        return def;
    }
    return s.value(key);
}

// INI-формат QSettings отдаёт "a, b" как QStringList, а "a" как QString. Приводим к одному виду.
QStringList readList(QSettings& s, const QString& key, const QStringList& def)
{
    // Пустой QStringList QSettings пишет как @Invalid(), поэтому храним строкой через запятую.
    const QVariant v = readOrInit(s, key, def.join(QStringLiteral(", ")));
    QStringList raw = v.typeId() == QMetaType::QStringList ? v.toStringList()
                                                           : v.toString().split(QLatin1Char(','));
    QStringList out;
    for (QString& item : raw) {
        item = item.trimmed();
        if (!item.isEmpty())
            out << item;
    }
    return out;
}

QString randomToken()
{
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
    return QString::fromLatin1(bytes.toHex());
}

} // namespace

QString Config::settingsPath()
{
    return openSettings()->fileName();
}

Config Config::load()
{
    Config c;
    auto s = openSettings();

    c.apiKey = qEnvironmentVariable("GEMINI_API_KEY", qEnvironmentVariable("GOOGLE_API_KEY"));
    if (c.apiKey.isEmpty())
        c.apiKey = s->value(QStringLiteral("gemini/apiKey")).toString();

    c.model = readOrInit(*s, QStringLiteral("gemini/model"), c.model).toString();
    c.baseUrl = readOrInit(*s, QStringLiteral("gemini/baseUrl"), c.baseUrl).toString();
    c.thinkingLevel = readOrInit(*s, QStringLiteral("gemini/thinkingLevel"), c.thinkingLevel).toString();
    c.maxOutputTokens = readOrInit(*s, QStringLiteral("gemini/maxOutputTokens"), c.maxOutputTokens).toInt();
    c.maxHistoryTurns = readOrInit(*s, QStringLiteral("gemini/maxHistoryTurns"), c.maxHistoryTurns).toInt();
    c.proxy = readOrInit(*s, QStringLiteral("network/proxy"), QString()).toString();

    c.screenshotHotkey = readOrInit(*s, QStringLiteral("hotkeys/screenshot"), c.screenshotHotkey).toString();
    c.chatHotkey = readOrInit(*s, QStringLiteral("hotkeys/chat"), c.chatHotkey).toString();
    c.screenshotMaxSide = readOrInit(*s, QStringLiteral("screen/maxSide"), c.screenshotMaxSide).toInt();

    c.bridgeEnabled = readOrInit(*s, QStringLiteral("bridge/enabled"), c.bridgeEnabled).toBool();
    c.bridgeHost = readOrInit(*s, QStringLiteral("bridge/host"), c.bridgeHost).toString();
    c.bridgePort = quint16(readOrInit(*s, QStringLiteral("bridge/port"), c.bridgePort).toUInt());
    c.bridgeAllowedSubnets = readList(*s, QStringLiteral("bridge/allowedSubnets"),
                                      {QStringLiteral("192.168.0.0/16"), QStringLiteral("10.0.0.0/8"),
                                       QStringLiteral("172.16.0.0/12")});
    c.bridgeToken = s->value(QStringLiteral("bridge/token")).toString();
    if (c.bridgeToken.size() < 32) {
        c.bridgeToken = randomToken();
        s->setValue(QStringLiteral("bridge/token"), c.bridgeToken);
    }

    c.haUrl = QUrl(readOrInit(*s, QStringLiteral("homeassistant/url"), QString()).toString());
    c.haToken = readOrInit(*s, QStringLiteral("homeassistant/token"), QString()).toString();
    c.haAllowedEntities = readList(*s, QStringLiteral("homeassistant/allowedEntities"), {});
    c.haConfirmDomains = readList(*s, QStringLiteral("homeassistant/confirmDomains"),
                                  {QStringLiteral("lock"), QStringLiteral("alarm_control_panel"),
                                   QStringLiteral("cover"), QStringLiteral("climate")});

    if (c.maxOutputTokens <= 0)
        c.maxOutputTokens = 8192;
    if (c.maxHistoryTurns <= 0)
        c.maxHistoryTurns = 20;
    if (c.screenshotMaxSide < 640)
        c.screenshotMaxSide = 2560;
    return c;
}

void Config::saveApiKey(const QString& key)
{
    apiKey = key.trimmed();
    openSettings()->setValue(QStringLiteral("gemini/apiKey"), apiKey);
}
