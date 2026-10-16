#pragma once

#include "Config.h"

#include <QObject>
#include <QStringList>

#include <memory>

class ChatWidget;
class GeminiClient;
class HomeAssistant;
class HotkeyService;
class LocalBridge;
class LocalTools;
class PetWidget;
class QMenu;
class QSystemTrayIcon;

// Точка сборки: создаёт модули и связывает их сигналами. Логики тут минимум,
// поэтому в свой существующий питомец проще всего перенести именно этот файл,
// заменив PetWidget своим виджетом.
class App : public QObject
{
    Q_OBJECT
public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    void start();
    void handleCommand(const QString& command);   // от второго экземпляра: chat | screenshot | quit

private:
    void wireGemini();
    void wirePet();
    void setupBridge();
    void setupHotkeys();
    void setupTray();

    void toggleChat();
    void openChat(bool takeFocus);
    void submit(const QString& text);
    void captureScreen();
    void resetConversation();
    void askApiKey();
    void notify(const QString& title, const QString& text);
    void react(const QStringList& lines);

    void pushEnvEvent(const QString& event);
    QString takeEnvPrefix();

    Config m_cfg;
    HomeAssistant* m_ha = nullptr;
    LocalTools* m_tools = nullptr;
    GeminiClient* m_gemini = nullptr;
    LocalBridge* m_bridge = nullptr;
    HotkeyService* m_hotkeys = nullptr;
    QSystemTrayIcon* m_tray = nullptr;

    std::unique_ptr<PetWidget> m_pet;
    std::unique_ptr<ChatWidget> m_chat;
    std::unique_ptr<QMenu> m_menu;

    QStringList m_envEvents;     // события «среды», уйдут модели с ближайшим сообщением
    QStringList m_startupNotes;  // предупреждения при старте, покажем при первом открытии чата
    bool m_bridgeAskBusy = false;
};
