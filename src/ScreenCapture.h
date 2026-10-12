#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

// Снимок экрана целиком в памяти. Файл на диск не пишется вообще, поэтому и удалять нечего:
// это надёжнее, чем «сохранить во временную папку и потом стереть» (после краша файл остался бы).
namespace ScreenCapture {

struct Result
{
    QByteArray data;      // PNG или JPEG
    QString mimeType;
    QSize size;
    QString error;        // не пусто, если снять не удалось
};

// Снимает экран, на котором сейчас курсор. maxSide ограничивает длинную сторону.
Result grabScreenUnderCursor(int maxSide);

} // namespace ScreenCapture
