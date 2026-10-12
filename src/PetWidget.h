#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class QScreen;
class SpeechBubble;

// Сам питомец: прозрачное окно без рамки поверх всех окон, в нём рисуется комета.
// Клик открывает чат, перетаскивание двигает, правый клик даёт меню.
// Живость: дыхание, моргание, глаза следят за курсором, хвост тянется против движения.
class PetWidget : public QWidget
{
    Q_OBJECT
public:
    enum class Mood { Idle, Thinking, Talking, Alert, Error };

    explicit PetWidget(QWidget* parent = nullptr);
    ~PetWidget() override;

    void setMood(Mood mood, int forMs = 0);
    void say(const QString& text, int ms = 0);
    void hop();
    void restorePosition();

    static QPixmap renderIcon(int size);

signals:
    void clicked();
    void tickled(int clicks);
    void dragFinished(const QString& where);
    void bubbleClicked();
    void contextMenuRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void moveEvent(QMoveEvent* e) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    void tick();
    void savePosition() const;
    void placeBubble();
    QString describePlace() const;

    Mood m_mood = Mood::Idle;
    QTimer m_moodReset;
    QTimer m_frame;
    QElapsedTimer m_clock;

    QPointF m_velocity;       // px/с, для хвоста
    QPoint m_lastPos;
    qint64 m_lastMoveMs = 0;
    QPointF m_look;           // куда смотрят глаза, [-1..1]
    qint64 m_nextBlinkMs = 2500;
    qint64 m_hopStartMs = -10'000;

    bool m_dragging = false;
    QPoint m_pressGlobal;
    QPoint m_pressWindowPos;
    const QScreen* m_dragStartScreen = nullptr;   // только для сравнения
    QList<qint64> m_clicks;

    SpeechBubble* m_bubble;
};
