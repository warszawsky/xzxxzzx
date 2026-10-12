#include "ChatWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

const char* kStyle = R"CSS(
QTextBrowser { background: transparent; border: none; color: #dbe7ff; font-size: 13px;
               selection-background-color: #1d4ed8; }
QPlainTextEdit { background: rgba(255,255,255,16); color: #f1f5ff; font-size: 13px;
                 border: 1px solid rgba(56,189,248,90); border-radius: 10px; padding: 4px 6px; }
QPlainTextEdit:focus { border-color: rgba(74,222,128,180); }
QToolButton { color: #cfe8ff; font-size: 15px; border: none; border-radius: 8px; padding: 3px 6px; }
QToolButton:hover { background: rgba(255,255,255,26); }
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
QScrollBar::handle:vertical { background: rgba(255,255,255,46); border-radius: 4px; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)CSS";

QToolButton* makeButton(QWidget* parent, const QString& text, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

// Markdown -> документ с подсветкой блоков кода и `инлайн-кода`.
void styleCode(QTextDocument& md)
{
    QList<QPair<int, int>> inlineRanges;
    for (QTextBlock b = md.begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat bf = b.blockFormat();
        if (bf.hasProperty(QTextFormat::BlockCodeFence) || bf.nonBreakableLines()) {
            bf.setBackground(QColor(255, 255, 255, 20));
            bf.setLeftMargin(4);
            QTextCursor(b).setBlockFormat(bf);
            continue;
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.charFormat().fontFixedPitch())
                inlineRanges << qMakePair(f.position(), f.length());
        }
    }
    QTextCharFormat code;
    code.setBackground(QColor(255, 255, 255, 24));
    code.setForeground(QColor(165, 243, 252));
    for (const auto& r : std::as_const(inlineRanges)) {
        QTextCursor c(&md);
        c.setPosition(r.first);
        c.setPosition(r.first + r.second, QTextCursor::KeepAnchor);
        c.mergeCharFormat(code);
    }
}

} // namespace

ChatWidget::ChatWidget(QWidget* parent) : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("Джеминик"));
    resize(380, 460);
    setMinimumSize(300, 260);
    setStyleSheet(QString::fromUtf8(kStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 12);
    root->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("✦ Джеминик"), this);
    title->setStyleSheet(QStringLiteral("color:#e6f6ff; font-weight:600; font-size:14px;"));
    m_title = title;
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#7dd3fc; font-size:11px;"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* shot = makeButton(this, QStringLiteral("📸"), tr("Анализ экрана. Текст из поля ввода уйдёт вопросом"));
    auto* copy = makeButton(this, QStringLiteral("⧉"), tr("Скопировать код из ответа (или весь ответ)"));
    auto* clear = makeButton(this, QStringLiteral("⟲"), tr("Новый диалог"));
    auto* close = makeButton(this, QStringLiteral("✕"), tr("Спрятать (Esc)"));
    header->addWidget(title);
    header->addWidget(m_status);
    header->addStretch();
    for (auto* b : {shot, copy, clear, close})
        header->addWidget(b);
    root->addLayout(header);

    m_view = new QTextBrowser(this);
    m_view->setOpenExternalLinks(true);
    m_view->document()->setDocumentMargin(4);
    m_view->document()->setIndentWidth(14);   // списки в узком окне без огромного отступа
    QPalette pal = m_view->palette();
    pal.setColor(QPalette::Link, QColor(103, 232, 249));
    m_view->setPalette(pal);
    root->addWidget(m_view, 1);

    auto* bottom = new QHBoxLayout;
    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Спроси что-нибудь… (/help)"));
    m_input->setTabChangesFocus(true);
    m_input->document()->setDocumentMargin(2);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->installEventFilter(this);
    m_send = makeButton(this, QStringLiteral("➤"), tr("Отправить (Enter)"));
    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_send, 0, Qt::AlignBottom);
    root->addLayout(bottom);
    adjustInputHeight();

    connect(m_input, &QPlainTextEdit::textChanged, this, &ChatWidget::adjustInputHeight);
    connect(m_send, &QToolButton::clicked, this, &ChatWidget::submit);
    connect(shot, &QToolButton::clicked, this, &ChatWidget::screenshotRequested);
    connect(copy, &QToolButton::clicked, this, &ChatWidget::copyAnswer);
    connect(clear, &QToolButton::clicked, this, &ChatWidget::clearRequested);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);

    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(40);
    connect(&m_renderTimer, &QTimer::timeout, this, &ChatWidget::renderSegment);
}

// ---------------------------------------------------------------------------------------------

void ChatWidget::popUpNear(const QRect& pet, bool takeFocus)
{
    if (!isVisible()) {
        QScreen* s = QGuiApplication::screenAt(pet.center());
        if (!s)
            s = QGuiApplication::primaryScreen();
        const QRect a = s->availableGeometry();
        int x = pet.center().x() > a.center().x() ? pet.left() - width() + 30 : pet.right() - 30;
        int y = pet.center().y() - height() / 2;
        x = qBound(a.left() + 8, x, a.right() - width() - 8);
        y = qBound(a.top() + 8, y, a.bottom() - height() - 8);
        move(x, y);
    }
    // Если открылись по хоткею, не отбираем фокус у IDE: человек продолжает печатать там.
    setAttribute(Qt::WA_ShowWithoutActivating, !takeFocus);
    show();
    raise();
    if (takeFocus) {
        activateWindow();
        m_input->setFocus();
    }
}

void ChatWidget::setStatus(const QString& text)
{
    m_status->setText(text);
}

void ChatWidget::setModelName(const QString& model)
{
    m_title->setToolTip(tr("Модель: %1").arg(model));
}

void ChatWidget::setBusy(bool busy)
{
    m_busy = busy;
    m_send->setText(busy ? QStringLiteral("■") : QStringLiteral("➤"));
    m_send->setToolTip(busy ? tr("Остановить") : tr("Отправить (Enter)"));
}

QString ChatWidget::takeInput()
{
    const QString text = m_input->toPlainText().trimmed();
    m_input->clear();
    return text;
}

void ChatWidget::submit()
{
    if (m_busy) {
        emit stopRequested();
        return;
    }
    const QString text = takeInput();
    if (!text.isEmpty())
        emit sendRequested(text);
}

void ChatWidget::adjustInputHeight()
{
    constexpr int kMaxLines = 5;
    const int lines = int(m_input->document()->size().height());
    m_input->setVerticalScrollBarPolicy(lines > kMaxLines ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    // строки + document margin + padding и рамка из стиля
    m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * qBound(1, lines, kMaxLines) + 4 + 8 + 2 + 4);
}

// ---------------------------------------------------------------------------------------------
// Сообщения

void ChatWidget::insertBlockText(const QString& text, const QString& color, bool alignRight, bool italic)
{
    QTextCursor c(m_view->document());
    c.movePosition(QTextCursor::End);
    QTextBlockFormat bf;
    bf.setAlignment(alignRight ? Qt::AlignRight : Qt::AlignLeft);
    bf.setTopMargin(alignRight ? 10 : 4);
    bf.setBottomMargin(2);
    QTextCharFormat cf;
    cf.setForeground(QColor(color));
    cf.setFontItalic(italic);
    if (m_view->document()->isEmpty())
        c.setBlockFormat(bf);
    else
        c.insertBlock(bf);
    c.insertText(text, cf);
}

void ChatWidget::addUserMessage(const QString& text, bool withScreenshot)
{
    insertBlockText((withScreenshot ? QStringLiteral("📸 ") : QString()) + text, QStringLiteral("#93c5fd"), true, false);
    scrollToBottomIfNeeded(true);
}

void ChatWidget::beginAssistant()
{
    m_lastAnswer.clear();
    m_streaming = true;
    startSegment();
}

void ChatWidget::appendAssistant(const QString& delta)
{
    if (!m_streaming)
        beginAssistant();
    m_segment += delta;
    m_lastAnswer += delta;
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void ChatWidget::endAssistant(const QStringList& sources)
{
    if (!m_streaming)
        return;
    if (!sources.isEmpty())
        m_segment += QStringLiteral("\n\n**Источники:** ") + sources.join(QStringLiteral(" · "));
    finishSegment();
    m_streaming = false;
}

// Ответ может прерываться заметками (вызовы функций), поэтому он рисуется кусками.
void ChatWidget::startSegment()
{
    m_segment.clear();
    QTextCursor c(m_view->document());
    c.movePosition(QTextCursor::End);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    m_segmentStart = c.position();
    renderSegment();
}

void ChatWidget::finishSegment()
{
    m_renderTimer.stop();
    if (!m_segment.isEmpty()) {
        renderSegment();
        return;
    }
    // Пустой кусок: убираем плейсхолдер «⋯» вместе с его строкой.
    QTextCursor c(m_view->document());
    c.setPosition(qMax(0, m_segmentStart - 1));
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    c.removeSelectedText();
}

// Перерисовывает только текущий кусок ответа, а не весь чат.
void ChatWidget::renderSegment()
{
    QScrollBar* sb = m_view->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 40;

    QTextCursor c(m_view->document());
    c.setPosition(m_segmentStart);
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    c.setBlockFormat(QTextBlockFormat());
    c.setCharFormat(QTextCharFormat());

    if (m_segment.isEmpty()) {
        QTextCharFormat dim;
        dim.setForeground(QColor(125, 211, 252));
        c.insertText(QStringLiteral("⋯"), dim);
    } else {
        QTextDocument md;
        md.setMarkdown(m_segment, QTextDocument::MarkdownDialectGitHub);
        styleCode(md);
        c.insertFragment(QTextDocumentFragment(&md));
    }
    scrollToBottomIfNeeded(atBottom);
}

void ChatWidget::addNote(const QString& text)
{
    if (m_streaming)
        finishSegment();
    insertBlockText(text, QStringLiteral("#94a3b8"), false, true);
    if (m_streaming)
        startSegment();
    scrollToBottomIfNeeded(true);
}

void ChatWidget::addError(const QString& text)
{
    insertBlockText(QStringLiteral("⚠ ") + text, QStringLiteral("#fca5a5"), false, false);
    scrollToBottomIfNeeded(true);
}

void ChatWidget::clearView()
{
    m_renderTimer.stop();
    m_view->clear();
    m_streaming = false;
    m_segment.clear();
    m_lastAnswer.clear();
    m_segmentStart = 0;
}

void ChatWidget::scrollToBottomIfNeeded(bool force)
{
    if (force)
        m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}

void ChatWidget::copyAnswer()
{
    if (m_lastAnswer.isEmpty()) {
        addNote(tr("Пока нечего копировать."));
        return;
    }
    static const QRegularExpression fence(QStringLiteral("```[^\\n]*\\n(.*?)```"),
                                          QRegularExpression::DotMatchesEverythingOption);
    QString code;
    for (auto it = fence.globalMatch(m_lastAnswer); it.hasNext();)
        code = it.next().captured(1);
    QGuiApplication::clipboard()->setText(code.isEmpty() ? m_lastAnswer : code);
    addNote(code.isEmpty() ? tr("Ответ скопирован.") : tr("Код из ответа скопирован."));
}

// ---------------------------------------------------------------------------------------------
// Оформление и мышь

void ChatWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QLinearGradient border(r.topLeft(), r.bottomRight());
    border.setColorAt(0.0, QColor(56, 189, 248, 170));
    border.setColorAt(1.0, QColor(74, 222, 128, 170));
    p.setPen(QPen(QBrush(border), 1.4));
    p.setBrush(QColor(14, 17, 32, 242));
    p.drawRoundedRect(r, 16, 16);
}

void ChatWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
        hide();
    else
        QWidget::keyPressEvent(e);
}

bool ChatWidget::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_input && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !(ke->modifiers() & Qt::ShiftModifier)) {
            submit();
            return true;
        }
        if (ke->key() == Qt::Key_Escape) {
            hide();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

// Окно без рамки двигается за любое пустое место (заголовок, поля).
void ChatWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
}

void ChatWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton)
        move(e->globalPosition().toPoint() - m_dragOffset);
}
