#include "PetWidget.h"

#include <QApplication>
#include <QCursor>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

namespace {

constexpr int kSide = 160;        // размер окна питомца
constexpr qreal kRadius = 34.0;   // радиус кометы (до кончика луча)
constexpr qreal kPi = 3.14159265358979323846;

// Четырёхлучевая звезда с вогнутыми сторонами, как на референсе.
QPainterPath starPath(qreal r)
{
    const qreal a = 0.10 * r, b = 0.44 * r;
    QPainterPath p;
    p.moveTo(0, -r);
    p.cubicTo(a, -b, b, -a, r, 0);
    p.cubicTo(b, a, a, b, 0, r);
    p.cubicTo(-a, b, -b, a, -r, 0);
    p.cubicTo(-b, -a, -a, -b, 0, -r);
    p.closeSubpath();
    return p;
}

QColor moodColor(PetWidget::Mood mood)
{
    switch (mood) {
    case PetWidget::Mood::Thinking: return QColor(99, 102, 241);
    case PetWidget::Mood::Talking: return QColor(20, 217, 138);
    case PetWidget::Mood::Alert: return QColor(251, 191, 36);
    case PetWidget::Mood::Error: return QColor(248, 113, 113);
    case PetWidget::Mood::Idle: break;
    }
    return QColor(34, 211, 238);
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(qBound(0, alpha, 255));
    return c;
}

struct CometState
{
    qreal t = 0;                       // секунды
    PetWidget::Mood mood = PetWidget::Mood::Idle;
    QPointF velocity;                  // px/с
    QPointF look;                      // направление взгляда
    bool blink = false;
    bool eyes = true;
    bool tail = true;
    bool sparkles = true;
};

void paintComet(QPainter& p, const QPointF& center, qreal r, const CometState& s)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(center);
    p.setPen(Qt::NoPen);

    const bool thinking = s.mood == PetWidget::Mood::Thinking;
    const QColor accent = moodColor(s.mood);

    // Хвост: в покое лениво смотрит вниз-влево, при движении вытягивается против скорости.
    if (s.tail) {
        const qreal speed = std::hypot(s.velocity.x(), s.velocity.y());
        QPointF dir;
        qreal len;
        if (speed > 40) {
            dir = -s.velocity / speed;
            len = qMin(2.2 * r, 0.9 * r + speed * 0.05);
        } else {
            const qreal ang = qDegreesToRadians(135.0 + 8.0 * std::sin(s.t * 1.3));
            dir = QPointF(std::cos(ang), std::sin(ang));
            len = 0.95 * r;
        }
        const QPointF perp(-dir.y(), dir.x());
        const qreal w = 0.5 * r;
        const QPointF tip = dir * len;
        QPainterPath tail;
        tail.moveTo(perp * w);
        tail.quadTo(dir * len * 0.45 + perp * w * 0.45, tip);
        tail.quadTo(dir * len * 0.45 - perp * w * 0.45, -perp * w);
        tail.closeSubpath();
        QLinearGradient g(QPointF(0, 0), tip);
        g.setColorAt(0.0, withAlpha(accent, 170));
        g.setColorAt(0.6, QColor(20, 217, 138, 60));
        g.setColorAt(1.0, QColor(20, 217, 138, 0));
        p.setBrush(g);
        p.drawPath(tail);
    }

    // Свечение, пульсирует. Когда думает, пульс чаще.
    const qreal pulse = thinking ? 0.5 + 0.5 * std::sin(s.t * 7.0) : 0.5 + 0.5 * std::sin(s.t * 2.2);
    QRadialGradient glow(QPointF(0, 0), r * 1.7);
    glow.setColorAt(0.0, withAlpha(accent, int(110 + 60 * pulse)));
    glow.setColorAt(0.55, withAlpha(accent, int(35 + 25 * pulse)));
    glow.setColorAt(1.0, withAlpha(accent, 0));
    p.setBrush(glow);
    p.drawEllipse(QPointF(0, 0), r * 1.7, r * 1.7);

    // Тело.
    p.save();
    const qreal scale = 1.0 + (thinking ? 0.05 : 0.025) * std::sin(s.t * (thinking ? 7.0 : 2.2));
    p.scale(scale, scale);
    if (thinking)
        p.rotate(6.0 * std::sin(s.t * 3.0));
    const QPainterPath body = starPath(r);

    QLinearGradient fill(-r, -r, r, r);
    fill.setColorAt(0.00, QColor(58, 63, 217));
    fill.setColorAt(0.35, QColor(11, 116, 240));
    fill.setColorAt(0.55, QColor(16, 184, 224));
    fill.setColorAt(0.78, QColor(20, 217, 138));
    fill.setColorAt(1.00, QColor(12, 143, 122));
    QLinearGradient edge(-r, -r, r, r);
    edge.setColorAt(0.0, QColor(56, 189, 248));
    edge.setColorAt(1.0, QColor(74, 222, 128));
    p.setPen(QPen(QBrush(edge), qMax<qreal>(1.2, r * 0.06), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(fill);
    p.drawPath(body);

    p.setPen(Qt::NoPen);
    QRadialGradient shine(QPointF(-0.25 * r, -0.3 * r), 0.75 * r);
    shine.setColorAt(0.0, QColor(255, 255, 255, 90));
    shine.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setBrush(shine);
    p.drawPath(body);

    // Глаза: моргают и смотрят на курсор (а когда думает, закатывает их вверх).
    if (s.eyes) {
        const QPointF look = thinking ? QPointF(0.3 * std::sin(s.t * 2.0), -1.0) : s.look;
        const qreal ew = 0.11 * r, eh = s.blink ? 0.025 * r : 0.17 * r;
        for (int side : {-1, 1}) {
            const QPointF eye(side * 0.2 * r + look.x() * 0.05 * r, -0.03 * r + look.y() * 0.05 * r);
            p.setBrush(QColor(8, 22, 64));
            p.drawEllipse(eye, ew, eh);
            if (!s.blink) {
                p.setBrush(QColor(255, 255, 255, 230));
                p.drawEllipse(eye + QPointF(-0.035 * r, -0.06 * r), 0.035 * r, 0.035 * r);
            }
        }
    }
    p.restore();

    // Искры вокруг: мерцают, а когда думает, кружат.
    if (s.sparkles) {
        struct Spark { qreal x, y, size, phase; QColor color; };
        static const Spark sparks[] = {
            {-0.78, -0.58, 0.14, 0.0, QColor(30, 136, 229)},
            {0.62, -0.72, 0.20, 1.7, QColor(34, 211, 166)},
            {-0.85, 0.48, 0.11, 3.1, QColor(56, 189, 248)},
            {0.66, 0.62, 0.14, 4.4, QColor(34, 211, 166)},
        };
        const qreal spin = thinking ? s.t * 90.0 : 0.0;
        for (const Spark& sp : sparks) {
            const qreal tw = 0.45 + 0.55 * std::abs(std::sin(s.t * 1.8 + sp.phase));
            p.save();
            p.rotate(spin);
            p.translate(sp.x * r * 1.35, sp.y * r * 1.35);
            p.setBrush(withAlpha(sp.color, int(255 * tw)));
            p.drawPath(starPath(sp.size * r * (0.7 + 0.3 * tw)));
            p.restore();
        }
    }
    p.restore();
}

std::unique_ptr<QSettings> settings()
{
    return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                       QStringLiteral("Geminik"), QStringLiteral("geminik"));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Пузырь с репликой над питомцем

class SpeechBubble : public QWidget
{
public:
    explicit SpeechBubble(PetWidget* pet) : QWidget(nullptr), m_pet(pet)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 10, 14, 18);
        m_label = new QLabel(this);
        m_label->setWordWrap(true);
        m_label->setTextFormat(Qt::PlainText);   // текст от модели, никакого HTML
        m_label->setMaximumWidth(260);
        m_label->setStyleSheet(QStringLiteral("color:#e6f6ff; font-size:13px;"));
        layout->addWidget(m_label);
        m_hide.setSingleShot(true);
        QObject::connect(&m_hide, &QTimer::timeout, this, &QWidget::hide);
        setCursor(Qt::PointingHandCursor);
    }

    void show(const QString& text, int ms)
    {
        m_label->setText(text);
        adjustSize();
        QWidget::show();
        raise();
        m_hide.start(ms);
    }

    bool pointsDown = true;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1, pointsDown ? 1 : 9, -1, pointsDown ? -9 : -1);
        QPainterPath path;
        path.addRoundedRect(box, 12, 12);
        const qreal cx = box.center().x();
        QPainterPath tip;
        if (pointsDown) {
            tip.moveTo(cx - 8, box.bottom() - 1);
            tip.lineTo(cx, box.bottom() + 8);
            tip.lineTo(cx + 8, box.bottom() - 1);
        } else {
            tip.moveTo(cx - 8, box.top() + 1);
            tip.lineTo(cx, box.top() - 8);
            tip.lineTo(cx + 8, box.top() + 1);
        }
        path = path.united(tip);
        p.setBrush(QColor(17, 20, 38, 235));
        p.setPen(QPen(QColor(56, 189, 248, 160), 1.2));
        p.drawPath(path);
    }

    void mousePressEvent(QMouseEvent*) override
    {
        hide();
        emit m_pet->bubbleClicked();
    }

private:
    PetWidget* m_pet;
    QLabel* m_label;
    QTimer m_hide;
};

// ---------------------------------------------------------------------------------------------

PetWidget::PetWidget(QWidget* parent) : QWidget(parent), m_bubble(new SpeechBubble(this))
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(kSide, kSide);
    setCursor(Qt::OpenHandCursor);
    setWindowTitle(QStringLiteral("Джеминик"));

    m_clock.start();
    m_frame.setInterval(33);
    connect(&m_frame, &QTimer::timeout, this, &PetWidget::tick);
    m_moodReset.setSingleShot(true);
    connect(&m_moodReset, &QTimer::timeout, this, [this] { m_mood = Mood::Idle; });
}

PetWidget::~PetWidget()
{
    delete m_bubble;
}

QPixmap PetWidget::renderIcon(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    CometState s;
    s.tail = false;
    s.sparkles = size >= 48;
    s.eyes = size >= 32;
    paintComet(p, QPointF(size / 2.0, size / 2.0), size * 0.3, s);
    return pm;
}

void PetWidget::setMood(Mood mood, int forMs)
{
    m_mood = mood;
    if (forMs > 0)
        m_moodReset.start(forMs);
    else
        m_moodReset.stop();
}

void PetWidget::say(const QString& text, int ms)
{
    if (ms <= 0)
        ms = qBound(2500, 2500 + 45 * int(text.size()), 9000);
    m_bubble->show(text, ms);
    placeBubble();
}

void PetWidget::hop()
{
    m_hopStartMs = m_clock.elapsed();
}

void PetWidget::restorePosition()
{
    const QPoint saved = settings()->value(QStringLiteral("pet/pos"), QPoint(-1, -1)).toPoint();
    if (QGuiApplication::screenAt(saved + QPoint(kSide / 2, kSide / 2))) {
        move(saved);
        return;
    }
    const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
    move(area.right() - kSide - 40, area.bottom() - kSide - 40);
}

void PetWidget::savePosition() const
{
    settings()->setValue(QStringLiteral("pet/pos"), pos());
}

// ---------------------------------------------------------------------------------------------
// Анимация

void PetWidget::tick()
{
    const qint64 now = m_clock.elapsed();

    // Скорость затухает, когда окно перестали двигать.
    if (now - m_lastMoveMs > 50)
        m_velocity *= 0.85;

    // Глаза плавно следят за курсором.
    const QPointF toCursor = QPointF(mapFromGlobal(QCursor::pos())) - QPointF(kSide / 2.0, kSide / 2.0);
    const qreal dist = std::hypot(toCursor.x(), toCursor.y());
    const QPointF target = dist > 1 ? toCursor / qMax<qreal>(dist, 120.0) : QPointF();
    m_look += (target - m_look) * 0.15;

    if (now > m_nextBlinkMs + 140)
        m_nextBlinkMs = now + 2500 + QRandomGenerator::global()->bounded(3500);

    update();
}

void PetWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const qint64 now = m_clock.elapsed();
    CometState s;
    s.t = now / 1000.0;
    s.mood = m_mood;
    s.velocity = m_velocity;
    s.look = m_look;
    s.blink = now >= m_nextBlinkMs && now < m_nextBlinkMs + 140;

    qreal dy = 3.0 * std::sin(s.t * 1.6);   // парит
    const qint64 hopMs = now - m_hopStartMs;
    if (hopMs >= 0 && hopMs < 450)
        dy -= 14.0 * std::sin(kPi * hopMs / 450.0);

    if (m_dragging) {
        // Пока несут, чуть сплющивается.
        p.translate(kSide / 2.0, kSide / 2.0 + dy);
        p.scale(1.08, 0.94);
        paintComet(p, QPointF(0, 0), kRadius, s);
    } else {
        paintComet(p, QPointF(kSide / 2.0, kSide / 2.0 + dy), kRadius, s);
    }
}

void PetWidget::showEvent(QShowEvent*)
{
    m_frame.start();
}

void PetWidget::hideEvent(QHideEvent*)
{
    m_frame.stop();
    m_bubble->hide();
}

// ---------------------------------------------------------------------------------------------
// Мышь

void PetWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_pressGlobal = e->globalPosition().toPoint();
        m_pressWindowPos = pos();
        m_dragging = false;
        m_dragStartScreen = screen();
    } else if (e->button() == Qt::RightButton) {
        emit contextMenuRequested(e->globalPosition().toPoint());
    }
}

void PetWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const QPoint delta = e->globalPosition().toPoint() - m_pressGlobal;
    if (!m_dragging) {
        if (delta.manhattanLength() < QApplication::startDragDistance())
            return;
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        raise();   // не прятаться под окном чата, пока несут
    }
    move(m_pressWindowPos + delta);
}

void PetWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    if (m_dragging) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        savePosition();
        emit dragFinished(describePlace());
        return;
    }

    // Быстрые клики подряд считаем «тыканьем», а не открытием чата.
    const qint64 now = m_clock.elapsed();
    m_clicks << now;
    while (!m_clicks.isEmpty() && now - m_clicks.first() > 1500)
        m_clicks.removeFirst();
    if (m_clicks.size() >= 3)
        emit tickled(int(m_clicks.size()));
    else
        emit clicked();
}

void PetWidget::moveEvent(QMoveEvent* e)
{
    const qint64 now = m_clock.elapsed();
    const qint64 dt = qMax<qint64>(1, now - m_lastMoveMs);
    if (dt < 200) {
        const QPointF inst = QPointF(e->pos() - e->oldPos()) * (1000.0 / dt);
        m_velocity = m_velocity * 0.6 + inst * 0.4;
    }
    m_lastMoveMs = now;
    if (m_bubble->isVisible())
        placeBubble();
}

void PetWidget::placeBubble()
{
    const QRect area = screen() ? screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QSize sz = m_bubble->sizeHint().expandedTo(m_bubble->size());
    const QPoint c = geometry().center();
    int x = qBound(area.left() + 4, c.x() - sz.width() / 2, area.right() - sz.width() - 4);
    int y = c.y() - int(kRadius * 1.6) - sz.height();
    m_bubble->pointsDown = y >= area.top();
    if (!m_bubble->pointsDown)
        y = c.y() + int(kRadius * 1.6);
    m_bubble->move(x, y);
    m_bubble->update();
}

QString PetWidget::describePlace() const
{
    const QScreen* s = screen();
    if (!s)
        return tr("куда-то");
    const QRect a = s->availableGeometry();
    const QPoint c = geometry().center();
    const qreal fx = qreal(c.x() - a.left()) / a.width();
    const qreal fy = qreal(c.y() - a.top()) / a.height();
    const int h = fx < 0.33 ? -1 : (fx > 0.67 ? 1 : 0);
    const int v = fy < 0.33 ? -1 : (fy > 0.67 ? 1 : 0);

    QString place;
    if (h && v)
        place = tr("в %1 %2 угол экрана").arg(h < 0 ? tr("левый") : tr("правый"), v < 0 ? tr("верхний") : tr("нижний"));
    else if (h)
        place = h < 0 ? tr("к левому краю экрана") : tr("к правому краю экрана");
    else if (v)
        place = v < 0 ? tr("к верхнему краю экрана") : tr("к нижнему краю экрана");
    else
        place = tr("в центр экрана");

    if (m_dragStartScreen && m_dragStartScreen != s)
        place = tr("на другой монитор, ") + place;
    return place;
}
