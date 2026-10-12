#include "App.h"

#include "ChatWidget.h"
#include "GeminiClient.h"
#include "HomeAssistant.h"
#include "HotkeyService.h"
#include "LocalBridge.h"
#include "LocalTools.h"
#include "PetWidget.h"
#include "ScreenCapture.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

namespace {

QString describeTool(const QString& name, const QJsonObject& args)
{
    QString icon = QStringLiteral("⚙");
    if (name == QLatin1String("set_timer")) icon = QStringLiteral("⏱");
    else if (name == QLatin1String("set_reminder")) icon = QStringLiteral("🔔");
    else if (name == QLatin1String("add_note")) icon = QStringLiteral("📝");
    else if (name.startsWith(QLatin1String("smart_home"))) icon = QStringLiteral("🏠");
    QString a = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    if (a.size() > 120)
        a = a.left(117) + QStringLiteral("…");
    return QStringLiteral("%1 %2 %3").arg(icon, name, a == QLatin1String("{}") ? QString() : a);
}

// Первая содержательная строка ответа, для пузыря над питомцем, когда чат закрыт.
QString preview(const QString& markdown)
{
    for (QString line : markdown.split(QLatin1Char('\n'))) {
        line.remove(QLatin1Char('*')).remove(QLatin1Char('`')).remove(QLatin1Char('#'));
        line = line.trimmed();
        if (!line.isEmpty())
            return line.size() > 140 ? line.left(139) + QStringLiteral("…") : line;
    }
    return {};
}

} // namespace

App::App(QObject* parent)
    : QObject(parent),
      m_cfg(Config::load()),
      m_pet(std::make_unique<PetWidget>()),
      m_chat(std::make_unique<ChatWidget>()),
      m_menu(std::make_unique<QMenu>())
{
    m_ha = new HomeAssistant(m_cfg, this);
    m_tools = new LocalTools(m_ha, this);
    m_gemini = new GeminiClient(m_cfg, m_tools, this);
    m_hotkeys = new HotkeyService(this);
}

App::~App() = default;

void App::start()
{
    QIcon icon;
    for (int size : {16, 32, 64, 128})
        icon.addPixmap(PetWidget::renderIcon(size));
    QApplication::setWindowIcon(icon);

    wireGemini();
    wirePet();
    setupTray();
    setupHotkeys();
    setupBridge();

    m_chat->setModelName(m_cfg.model);
    m_pet->restorePosition();
    m_pet->show();

    if (m_gemini->hasApiKey()) {
        m_gemini->warmUp();
        m_pet->say(tr("Привет! Я Джеминик ✦ Кликни, чтобы поболтать."), 4000);
    } else {
        QTimer::singleShot(500, this, &App::askApiKey);
    }
}

void App::handleCommand(const QString& command)
{
    if (command == QLatin1String("screenshot"))
        captureScreen();
    else if (command == QLatin1String("quit"))
        QCoreApplication::quit();
    else
        toggleChat();
}

// ---------------------------------------------------------------------------------------------
// Связка модулей

void App::wireGemini()
{
    connect(m_gemini, &GeminiClient::replyStarted, this, [this] {
        m_chat->setBusy(true);
        m_chat->setStatus(tr("думаю…"));
        m_chat->beginAssistant();
        m_pet->setMood(PetWidget::Mood::Thinking);
    });
    connect(m_gemini, &GeminiClient::textDelta, this, [this](const QString& delta) {
        m_chat->appendAssistant(delta);
        m_pet->setMood(PetWidget::Mood::Talking);
    });
    connect(m_gemini, &GeminiClient::toolInvoked, this, [this](const QString& name, const QJsonObject& args) {
        m_chat->addNote(describeTool(name, args));
    });
    connect(m_gemini, &GeminiClient::replyFinished, this, [this](const QString& text, const QStringList& sources) {
        m_chat->endAssistant(sources);
        m_chat->setBusy(false);
        m_chat->setStatus({});
        m_pet->setMood(PetWidget::Mood::Idle);
        if (!m_chat->isVisible())
            m_pet->say(preview(text) + tr("  (кликни, чтобы открыть)"));
    });
    connect(m_gemini, &GeminiClient::replyFailed, this, [this](const QString& message) {
        m_chat->endAssistant();
        m_chat->addError(message);
        m_chat->setBusy(false);
        m_chat->setStatus({});
        m_pet->setMood(PetWidget::Mood::Error, 3000);
        if (!m_chat->isVisible())
            m_pet->say(QStringLiteral("⚠ ") + message);
    });

    connect(m_tools, &LocalTools::alarm, this, [this](const QString& title, const QString& text) {
        notify(title, text);
        m_chat->addNote(title + QStringLiteral(": ") + text);
    });
    // Подтверждение действий с замками, сигнализацией и т.п. Неблокирующий диалог:
    // модель ждёт, остальное приложение живёт.
    connect(m_tools, &LocalTools::confirmationNeeded, this,
            [](const QString& question, const std::function<void(bool)>& answer) {
                auto* box = new QMessageBox(QMessageBox::Question, tr("Джеминик"), question,
                                            QMessageBox::Yes | QMessageBox::No);
                box->setDefaultButton(QMessageBox::No);
                box->setAttribute(Qt::WA_DeleteOnClose);
                box->setWindowFlag(Qt::WindowStaysOnTopHint);
                connect(box, &QMessageBox::finished, box, [box, answer](int) {
                    answer(box->clickedButton() == box->button(QMessageBox::Yes));
                });
                box->open();
            });
}

void App::wirePet()
{
    connect(m_pet.get(), &PetWidget::clicked, this, &App::toggleChat);
    connect(m_pet.get(), &PetWidget::bubbleClicked, this, [this] { openChat(true); });
    connect(m_pet.get(), &PetWidget::contextMenuRequested, this, [this](const QPoint& at) { m_menu->popup(at); });

    // Реакции на «среду»: мгновенная локальная реплика (бесплатно и без задержки),
    // а само событие уйдёт модели с ближайшим сообщением, чтобы она тоже была в курсе.
    connect(m_pet.get(), &PetWidget::dragFinished, this, [this](const QString& where) {
        pushEnvEvent(tr("пользователь перетащил тебя мышкой %1").arg(where));
        react({tr("Уиии! 🚀"), tr("Новое место, новый вайб."), tr("Эй, я же не курсор!"),
               tr("Пересчитываю координаты… готово."), tr("О, отсюда вид получше.")});
    });
    connect(m_pet.get(), &PetWidget::tickled, this, [this](int clicks) {
        pushEnvEvent(tr("пользователь быстро потыкал в тебя курсором (%1 раза подряд)").arg(clicks));
        m_pet->hop();
        react({tr("Хи-хи, щекотно!"), tr("Ай! Я комета, а не кнопка 😤"), tr("Перегрев сенсоров! 🔥"),
               tr("Ещё раз, и я улечу в другой угол.")});
    });

    connect(m_chat.get(), &ChatWidget::sendRequested, this, &App::submit);
    connect(m_chat.get(), &ChatWidget::screenshotRequested, this, &App::captureScreen);
    connect(m_chat.get(), &ChatWidget::clearRequested, this, &App::resetConversation);
    connect(m_chat.get(), &ChatWidget::stopRequested, this, [this] {
        m_gemini->cancel();
        m_chat->endAssistant();
        m_chat->addNote(tr("Остановил."));
        m_chat->setBusy(false);
        m_chat->setStatus({});
        m_pet->setMood(PetWidget::Mood::Idle);
    });
}

void App::setupTray()
{
    const auto add = [this](const QString& text, auto slot) {
        QAction* a = m_menu->addAction(text);
        connect(a, &QAction::triggered, this, slot);
    };
    add(tr("💬 Чат\t%1").arg(m_cfg.chatHotkey), [this] { toggleChat(); });
    add(tr("📸 Анализ экрана\t%1").arg(m_cfg.screenshotHotkey), [this] { captureScreen(); });
    add(tr("⟲ Новый диалог"), [this] { resetConversation(); });
    m_menu->addSeparator();
    add(tr("🔑 API-ключ…"), [this] { askApiKey(); });
    add(tr("⚙ Открыть конфиг"), [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Config::settingsPath())); });
    if (m_cfg.bridgeEnabled) {
        add(tr("🔗 Скопировать токен моста"), [this] {
            QGuiApplication::clipboard()->setText(m_cfg.bridgeToken);
            m_pet->say(tr("Токен моста в буфере обмена. Никому не показывай."));
        });
    }
    m_menu->addSeparator();
    add(tr("Выход"), [] { QCoreApplication::quit(); });

    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
    m_tray->setToolTip(tr("Джеминик"));
    m_tray->setContextMenu(m_menu.get());
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            toggleChat();
    });
    m_tray->show();
}

void App::setupHotkeys()
{
    QString shotError, chatError;
    m_hotkeys->bind(m_cfg.screenshotHotkey, [this] { captureScreen(); }, &shotError);
    m_hotkeys->bind(m_cfg.chatHotkey, [this] { toggleChat(); }, &chatError);
    if (!shotError.isEmpty())
        m_startupNotes << tr("Хоткей %1 не работает: %2").arg(m_cfg.screenshotHotkey, shotError);
    if (!chatError.isEmpty() && chatError != shotError)
        m_startupNotes << tr("Хоткей %1 не работает: %2").arg(m_cfg.chatHotkey, chatError);
}

// HTTP API для умного дома и скриптов. Описание эндпоинтов и примеры curl в README.
void App::setupBridge()
{
    if (!m_cfg.bridgeEnabled)
        return;
    m_bridge = new LocalBridge(m_cfg, this);

    m_bridge->addRoute("GET", "/api/status", [this](const QJsonObject&, const LocalBridge::Respond& respond) {
        respond(200, {{QStringLiteral("name"), QStringLiteral("geminik")},
                      {QStringLiteral("model"), m_cfg.model},
                      {QStringLiteral("busy"), m_gemini->isBusy()}});
    });

    // Уведомление от умного дома или скрипта: пузырь над питомцем + системное уведомление.
    m_bridge->addRoute("POST", "/api/notify", [this](const QJsonObject& req, const LocalBridge::Respond& respond) {
        const QString text = req.value(QStringLiteral("text")).toString().trimmed().left(500);
        if (text.isEmpty())
            return respond(400, {{QStringLiteral("error"), QStringLiteral("text is required")}});
        const QString title = req.value(QStringLiteral("title")).toString(tr("Умный дом")).left(80);
        notify(title, text);
        pushEnvEvent(tr("пришло уведомление «%1»: %2").arg(title, text));
        respond(200, {{QStringLiteral("ok"), true}});
    });

    // Вопрос к модели. Без истории и без функций: снаружи нельзя управлять таймерами
    // и умным домом через модель, только получить текст.
    m_bridge->addRoute("POST", "/api/ask", [this](const QJsonObject& req, const LocalBridge::Respond& respond) {
        const QString text = req.value(QStringLiteral("text")).toString().trimmed().left(4000);
        if (text.isEmpty())
            return respond(400, {{QStringLiteral("error"), QStringLiteral("text is required")}});
        if (m_bridgeAskBusy)
            return respond(429, {{QStringLiteral("error"), QStringLiteral("busy, retry later")}});
        m_bridgeAskBusy = true;
        m_gemini->ask(text, [this, respond](const QString& reply, const QString& error) {
            m_bridgeAskBusy = false;
            if (error.isEmpty())
                respond(200, {{QStringLiteral("reply"), reply}});
            else
                respond(502, {{QStringLiteral("error"), error}});
        });
    });

    // Локальные команды без модели: {"text": "/timer 10m стирка"}.
    m_bridge->addRoute("POST", "/api/command", [this](const QJsonObject& req, const LocalBridge::Respond& respond) {
        QString reply;
        if (m_tools->handleCommand(req.value(QStringLiteral("text")).toString(), &reply))
            respond(200, {{QStringLiteral("reply"), reply}});
        else
            respond(400, {{QStringLiteral("error"), QStringLiteral("text must start with /")}});
    });

    QString error;
    if (!m_bridge->start(&error)) {
        m_startupNotes << error;
        return;
    }
    const QString host = m_cfg.bridgeHost;
    if (host != QLatin1String("127.0.0.1") && host != QLatin1String("::1") && host != QLatin1String("localhost"))
        m_startupNotes << tr("Мост открыт в локальную сеть: %1. Трафик не шифруется, токен держи в секрете "
                             "и не пробрасывай порт на роутере наружу.").arg(m_bridge->address());
}

// ---------------------------------------------------------------------------------------------
// Действия

void App::toggleChat()
{
    if (m_chat->isVisible())
        m_chat->hide();
    else
        openChat(true);
}

void App::openChat(bool takeFocus)
{
    m_chat->popUpNear(m_pet->frameGeometry(), takeFocus);
    for (const QString& note : std::as_const(m_startupNotes))
        m_chat->addNote(note);
    m_startupNotes.clear();
}

void App::submit(const QString& raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return;

    if (text == QLatin1String("/clear") || text == QLatin1String("/new")) {
        resetConversation();
        return;
    }

    bool web = false;
    QString question = text;
    if (text.startsWith(QLatin1String("/web"))) {
        web = true;
        question = text.mid(4).trimmed();
        if (question.isEmpty()) {
            m_chat->addNote(tr("Формат: /web вопрос"));
            return;
        }
    } else if (QString reply; m_tools->handleCommand(text, &reply)) {
        m_chat->addUserMessage(text, false);
        m_chat->beginAssistant();
        m_chat->appendAssistant(reply);
        m_chat->endAssistant();
        return;
    }

    if (!m_gemini->hasApiKey()) {
        m_chat->addError(tr("Нет API-ключа."));
        askApiKey();
        return;
    }
    if (m_gemini->isBusy()) {
        m_chat->addNote(tr("Я ещё отвечаю. ■ остановит."));
        return;
    }
    m_chat->addUserMessage(web ? QStringLiteral("🔎 ") + question : question, false);
    m_gemini->send(takeEnvPrefix() + question, {}, web);
}

// Хоткей → снимок экрана в память → в Gemini вместе с вопросом → ответ в чате.
// Если в поле ввода был текст, он уходит вопросом к скриншоту.
void App::captureScreen()
{
    if (!m_gemini->hasApiKey()) {
        openChat(true);
        m_chat->addError(tr("Нет API-ключа."));
        return;
    }
    if (m_gemini->isBusy()) {
        m_pet->say(tr("Секунду, ещё думаю над прошлым."));
        return;
    }

    const QString question = m_chat->takeInput();
    const bool chatWasVisible = m_chat->isVisible();
    if (chatWasVisible)
        m_chat->hide();   // чтобы чат не закрывал собой то, что надо разобрать

    QTimer::singleShot(chatWasVisible ? 150 : 0, this, [this, question] {
        const ScreenCapture::Result shot = ScreenCapture::grabScreenUnderCursor(m_cfg.screenshotMaxSide);
        openChat(false);
        if (!shot.error.isEmpty()) {
            m_chat->addError(shot.error);
            return;
        }
        m_chat->addUserMessage(question.isEmpty() ? tr("Что у меня на экране?") : question, true);
        const QString prompt = QStringLiteral("[скриншот %1×%2]\n").arg(shot.size.width()).arg(shot.size.height())
            + (question.isEmpty() ? tr("Посмотри на мой экран и помоги с тем, чем я сейчас занят.") : question);
        m_gemini->send(takeEnvPrefix() + prompt, Attachment{shot.data, shot.mimeType});
    });
}

void App::resetConversation()
{
    m_gemini->resetConversation();
    m_envEvents.clear();
    m_chat->clearView();
    m_chat->setBusy(false);
    m_chat->setStatus({});
    m_chat->addNote(tr("Новый диалог, старый забыт."));
    m_pet->setMood(PetWidget::Mood::Idle);
}

void App::askApiKey()
{
    bool ok = false;
    const QString key = QInputDialog::getText(
        nullptr, tr("Джеминик: ключ Gemini API"),
        tr("Вставь API-ключ из https://aistudio.google.com/apikey\n"
           "Он сохранится в %1\n(переменная окружения GEMINI_API_KEY главнее файла)").arg(Config::settingsPath()),
        QLineEdit::Password, QString(), &ok);
    if (!ok || key.trimmed().isEmpty()) {
        if (!m_gemini->hasApiKey())
            m_pet->say(tr("Без ключа я умею только /timer, /remind и /note. Ключ: правый клик → «API-ключ…»"), 6000);
        return;
    }
    m_cfg.saveApiKey(key);
    m_gemini->setApiKey(m_cfg.apiKey);
    m_gemini->warmUp();
    m_pet->say(tr("Ключ принят. Я на связи ✦"));
}

void App::notify(const QString& title, const QString& text)
{
    m_pet->say(title + QLatin1Char('\n') + text, 8000);
    m_pet->setMood(PetWidget::Mood::Alert, 4000);
    m_pet->hop();
    if (m_tray && QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(title, text, QSystemTrayIcon::Information, 8000);
}

void App::react(const QStringList& lines)
{
    if (!lines.isEmpty())
        m_pet->say(lines.at(QRandomGenerator::global()->bounded(int(lines.size()))), 2200);
}

void App::pushEnvEvent(const QString& event)
{
    m_envEvents << event;
    while (m_envEvents.size() > 5)
        m_envEvents.removeFirst();
}

QString App::takeEnvPrefix()
{
    if (m_envEvents.isEmpty())
        return {};
    const QString prefix = QStringLiteral("[среда: %1]\n").arg(m_envEvents.join(QStringLiteral("; ")));
    m_envEvents.clear();
    return prefix;
}
