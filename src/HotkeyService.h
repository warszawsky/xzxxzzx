#pragma once

#include <QObject>

#include <functional>

// Глобальные хоткеи (срабатывают, даже когда фокус в IDE или браузере).
// Обёртка над QHotkey: Windows (RegisterHotKey), macOS (Carbon), X11 (XGrabKey).
// Если собрано без QHotkey или ОС не даёт хоткей, bind() вернёт false с причиной.
class HotkeyService : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    bool bind(const QString& sequence, std::function<void()> action, QString* error);
};
