#include "HotkeyService.h"

#include <QGuiApplication>
#include <QKeySequence>

#ifdef GEMINIK_HAVE_QHOTKEY
#include <qhotkey.h>
#endif

bool HotkeyService::bind(const QString& sequence, std::function<void()> action, QString* error)
{
    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.isEmpty()) {
        *error = tr("Не понял сочетание «%1»").arg(sequence);
        return false;
    }
#ifdef GEMINIK_HAVE_QHOTKEY
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        *error = tr("Wayland не даёт приложениям глобальные хоткеи. Повесь в настройках DE "
                    "системное сочетание на команду «geminik --screenshot».");
        return false;
    }
    auto* hotkey = new QHotkey(seq, true, this);
    if (!hotkey->isRegistered()) {
        *error = tr("Сочетание %1 занято другой программой. Поменяй его в конфиге.").arg(sequence);
        delete hotkey;
        return false;
    }
    connect(hotkey, &QHotkey::activated, this, [action] { action(); });
    return true;
#else
    Q_UNUSED(action);
    *error = tr("Собрано без глобальных хоткеев (GEMINIK_GLOBAL_HOTKEYS=OFF).");
    return false;
#endif
}
