#include "ScreenCapture.h"

#include <QBuffer>
#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>

namespace ScreenCapture {

namespace {

// macOS без разрешения «Запись экрана» и Wayland отдают чёрную картинку вместо ошибки.
bool looksBlank(const QImage& img)
{
    const int stepX = qMax(1, img.width() / 24);
    const int stepY = qMax(1, img.height() / 24);
    for (int y = stepY / 2; y < img.height(); y += stepY)
        for (int x = stepX / 2; x < img.width(); x += stepX)
            if ((img.pixel(x, y) & 0x00FFFFFF) != 0)
                return false;
    return true;
}

QByteArray encode(const QImage& img, const char* format, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, format, quality);
    return bytes;
}

} // namespace

Result grabScreenUnderCursor(int maxSide)
{
    Result r;
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        r.error = QObject::tr("На Wayland приложения не могут молча снимать экран, это ограничение ОС. "
                              "Запусти с QT_QPA_PLATFORM=xcb в X11-сессии или используй Windows/macOS.");
        return r;
    }

    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        r.error = QObject::tr("Не нашёл ни одного экрана.");
        return r;
    }

    QImage img = screen->grabWindow(0).toImage();
    if (img.isNull() || looksBlank(img)) {
        r.error = QObject::tr("Снимок пустой. На macOS дай доступ: Настройки → Конфиденциальность → Запись экрана.");
        return r;
    }

    if (qMax(img.width(), img.height()) > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_RGB888);   // альфа-канал не нужен, PNG станет меньше

    // PNG сохраняет мелкий текст и код без артефактов. Если вышло слишком тяжело
    // (фото, видео на экране), откатываемся на JPEG: лимит запроса Gemini 20 МБ с учётом base64.
    r.data = encode(img, "PNG", -1);
    r.mimeType = QStringLiteral("image/png");
    if (r.data.size() > 6 * 1024 * 1024) {
        r.data = encode(img, "JPEG", 90);
        r.mimeType = QStringLiteral("image/jpeg");
    }
    r.size = img.size();
    return r;
}

} // namespace ScreenCapture
