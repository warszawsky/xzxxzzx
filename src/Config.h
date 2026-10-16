#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Все настройки приложения. Живут в INI-файле пользователя (путь: Config::settingsPath()),
// при первом запуске файл заполняется значениями по умолчанию, дальше правится руками.
// GEMINI_API_KEY из окружения перекрывает ключ из файла.
struct Config
{
    // --- Gemini ---
    QString apiKey;
    QString model = QStringLiteral("gemini-flash-latest");
    QString baseUrl = QStringLiteral("https://generativelanguage.googleapis.com/v1beta");
    QString thinkingLevel = QStringLiteral("low");   // пусто = не передавать; minimal|low|medium|high
    int maxOutputTokens = 8192;
    int maxHistoryTurns = 20;                       // сколько реплик пользователя помнить
    QString proxy;                                  // socks5://host:port или http://host:port

    // --- Экран и хоткеи ---
    QString screenshotHotkey = QStringLiteral("Ctrl+Alt+G");
    QString chatHotkey = QStringLiteral("Ctrl+Alt+J");
    int screenshotMaxSide = 2560;                   // длинная сторона снимка, px

    // --- Локальный мост (HTTP API для умного дома и других сервисов) ---
    bool bridgeEnabled = true;
    QString bridgeHost = QStringLiteral("127.0.0.1");
    quint16 bridgePort = 8765;
    QString bridgeToken;                            // генерируется при первом запуске
    QStringList bridgeAllowedSubnets;

    // --- Home Assistant ---
    QUrl haUrl;                                     // например http://homeassistant.local:8123
    QString haToken;                                // long-lived access token
    QStringList haAllowedEntities;                  // light.*, switch.desk_lamp, ...
    QStringList haConfirmDomains;                   // домены, где нужно подтверждение человеком

    static Config load();
    static QString settingsPath();
    void saveApiKey(const QString& key);
};
