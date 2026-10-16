#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTextBrowser;
class QToolButton;

// Всплывающее окно чата рядом с питомцем. Ответ модели рендерится как Markdown
// и перерисовывается по мере стриминга (не чаще раза в 40 мс).
class ChatWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ChatWidget(QWidget* parent = nullptr);

    void popUpNear(const QRect& petGeometry, bool takeFocus);
    void setStatus(const QString& text);
    void setModelName(const QString& model);
    void setBusy(bool busy);
    QString takeInput();

    void addUserMessage(const QString& text, bool withScreenshot);
    void beginAssistant();
    void appendAssistant(const QString& delta);
    void endAssistant(const QStringList& sources = {});
    void addNote(const QString& text);
    void addError(const QString& text);
    void clearView();

signals:
    void sendRequested(const QString& text);
    void stopRequested();
    void screenshotRequested();
    void clearRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void submit();
    void startSegment();
    void finishSegment();
    void renderSegment();
    void insertBlockText(const QString& text, const QString& color, bool alignRight, bool italic);
    void scrollToBottomIfNeeded(bool force);
    void copyAnswer();
    void adjustInputHeight();

    QLabel* m_title;
    QLabel* m_status;
    QTextBrowser* m_view;
    QPlainTextEdit* m_input;
    QToolButton* m_send;

    bool m_busy = false;
    bool m_streaming = false;
    int m_segmentStart = 0;      // позиция в документе, с которой рисуется текущий кусок ответа
    QString m_segment;           // markdown текущего куска (между вызовами функций)
    QString m_lastAnswer;        // весь последний ответ, для кнопки «копировать»
    QTimer m_renderTimer;
    QPoint m_dragOffset;
};
