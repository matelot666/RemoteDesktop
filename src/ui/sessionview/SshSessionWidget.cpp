#include "SshSessionWidget.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFontDatabase>
#include <QScrollBar>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <vterm_keycodes.h>

SshSessionWidget::SshSessionWidget(const ConnectionEntry &entry,
                                   const QString &username, const QString &password,
                                   const QString &privateKey,
                                   QWidget *parent)
    : QWidget(parent), m_entry(entry)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_OpaquePaintEvent);

    // Monospace font
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSize(13);
    calculateCellSize();
    initFontVariants();

    // Allocate initial back buffer so damage callbacks before first resizeEvent work
    m_backBuffer = QPixmap(m_cols * m_cellWidth, m_rows * m_cellHeight);
    m_backBuffer.fill(QColor(30, 30, 30));

    setupVterm();

    m_session = new SshSession(entry, username, password, privateKey, m_cols, m_rows);
    m_workerThread = new QThread(this);
    m_session->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_session, &SshSession::start);
    connect(m_session, &SshSession::connected, this, &SshSessionWidget::sessionConnected);
    connect(m_session, &SshSession::disconnected, this, [this]() {
        emit sessionDisconnected();
    });
    connect(m_session, &SshSession::errorOccurred, this, &SshSessionWidget::sessionError);
    connect(m_session, &SshSession::reconnectStatus, this, [this](const QString &msg) {
        if (msg.isEmpty()) {
            m_reconnectOverlay->hide();
        } else {
            m_reconnectOverlay->setText(msg);
            m_reconnectOverlay->setGeometry(rect());
            m_reconnectOverlay->show();
            m_reconnectOverlay->raise();
        }
    });
    connect(m_session, &SshSession::dataReceived,
            this, &SshSessionWidget::onDataReceived, Qt::QueuedConnection);
    connect(this, &SshSessionWidget::writeRequested,
            m_session, &SshSession::write, Qt::QueuedConnection);
    connect(this, &SshSessionWidget::resizeRequested,
            m_session, &SshSession::requestPtyResize, Qt::QueuedConnection);
    connect(m_workerThread, &QThread::finished, m_session, &QObject::deleteLater);

    // Cursor blink timer — only invalidates the cursor cell, not the whole widget
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(530);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_cursorVisible = !m_cursorVisible;
        if (m_vterm) {
            VTermPos pos;
            vterm_state_get_cursorpos(vterm_obtain_state(m_vterm), &pos);
            update(QRect(pos.col * m_cellWidth, pos.row * m_cellHeight,
                         m_cellWidth, m_cellHeight));
        }
    });
    m_blinkTimer->start();

    // Reconnect overlay (hidden by default)
    m_reconnectOverlay = new QLabel(this);
    m_reconnectOverlay->setAlignment(Qt::AlignCenter);
    m_reconnectOverlay->setWordWrap(true);
    m_reconnectOverlay->setStyleSheet(
        QStringLiteral("QLabel { background-color: rgba(0, 0, 0, 180); color: white; "
                        "font-size: 16px; padding: 40px; }"));
    m_reconnectOverlay->hide();
}

SshSessionWidget::~SshSessionWidget()
{
    disconnectSession();
    destroyVterm();
}

void SshSessionWidget::connectSession()
{
    m_workerThread->start();
}

void SshSessionWidget::disconnectSession()
{
    if (m_session) {
        m_session->stop();
    }
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        if (!m_workerThread->wait(30000)) {
            qWarning() << "SSH worker thread didn't finish in 30s, terminating";
            m_workerThread->terminate();
            m_workerThread->wait();
        }
    }
    m_session = nullptr;
}

void SshSessionWidget::setupVterm()
{
    m_vterm = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vterm, 1);

    vterm_output_set_callback(m_vterm, vtermOutputCallback, this);

    m_vtermScreen = vterm_obtain_screen(m_vterm);

    // libvterm stores a POINTER to the callbacks struct (does not copy),
    // so it must persist for the lifetime of the vterm instance.
    m_vtermCallbacks = {};
    m_vtermCallbacks.damage = [](VTermRect rect, void *user) -> int {
        auto *self = static_cast<SshSessionWidget *>(user);
        self->renderCells(rect);
        int x = rect.start_col * self->m_cellWidth;
        int y = rect.start_row * self->m_cellHeight;
        int w = (rect.end_col - rect.start_col) * self->m_cellWidth;
        int h = (rect.end_row - rect.start_row) * self->m_cellHeight;
        self->update(QRect(x, y, w, h));
        return 0;
    };
    m_vtermCallbacks.movecursor = [](VTermPos newPos, VTermPos oldPos, int, void *user) -> int {
        auto *self = static_cast<SshSessionWidget *>(user);
        // Invalidate old and new cursor positions
        self->update(QRect(oldPos.col * self->m_cellWidth, oldPos.row * self->m_cellHeight,
                           self->m_cellWidth, self->m_cellHeight));
        self->update(QRect(newPos.col * self->m_cellWidth, newPos.row * self->m_cellHeight,
                           self->m_cellWidth, self->m_cellHeight));
        return 0;
    };
    m_vtermCallbacks.bell = [](void *) -> int { return 0; };

    vterm_screen_set_callbacks(m_vtermScreen, &m_vtermCallbacks, this);
    vterm_screen_reset(m_vtermScreen, 1);
}

void SshSessionWidget::destroyVterm()
{
    if (m_vterm) {
        vterm_free(m_vterm);
        m_vterm = nullptr;
        m_vtermScreen = nullptr;
    }
}

void SshSessionWidget::vtermOutputCallback(const char *s, size_t len, void *user)
{
    auto *self = static_cast<SshSessionWidget *>(user);
    emit self->writeRequested(QByteArray(s, static_cast<int>(len)));
}

void SshSessionWidget::onDataReceived(const QByteArray &data)
{
    if (m_vterm) {
        vterm_input_write(m_vterm, data.constData(), data.size());
        vterm_screen_flush_damage(m_vtermScreen);
        // No update() here — damage callback already issued targeted updates
    }
}

void SshSessionWidget::calculateCellSize()
{
    QFontMetrics fm(m_font);
    m_cellWidth = fm.horizontalAdvance('M');
    m_cellHeight = fm.height();
    m_fontAscent = fm.ascent();
}

void SshSessionWidget::initFontVariants()
{
    m_fontBold = m_font;
    m_fontBold.setBold(true);

    m_fontItalic = m_font;
    m_fontItalic.setItalic(true);

    m_fontBoldItalic = m_font;
    m_fontBoldItalic.setBold(true);
    m_fontBoldItalic.setItalic(true);

    m_fontUnderline = m_font;
    m_fontUnderline.setUnderline(true);

    m_fontBoldUnderline = m_font;
    m_fontBoldUnderline.setBold(true);
    m_fontBoldUnderline.setUnderline(true);

    m_fontStrike = m_font;
    m_fontStrike.setStrikeOut(true);
}

const QFont &SshSessionWidget::fontForAttrs(unsigned bold, unsigned italic,
                                             unsigned underline, unsigned strike) const
{
    if (strike) return m_fontStrike;
    if (bold && italic) return m_fontBoldItalic;
    if (bold && underline) return m_fontBoldUnderline;
    if (bold) return m_fontBold;
    if (italic) return m_fontItalic;
    if (underline) return m_fontUnderline;
    return m_font;
}

void SshSessionWidget::renderCells(const VTermRect &rect)
{
    if (m_backBuffer.isNull() || !m_vtermScreen)
        return;

    QPainter painter(&m_backBuffer);
    painter.setFont(m_font);

    for (int row = rect.start_row; row < rect.end_row; row++) {
        int col = rect.start_col;
        while (col < rect.end_col) {
            VTermScreenCell cell;
            VTermPos pos = {row, col};
            vterm_screen_get_cell(m_vtermScreen, pos, &cell);

            int cellWidth = cell.width > 0 ? cell.width : 1;
            int x = col * m_cellWidth;
            int y = row * m_cellHeight;

            QColor bgColor = vtermColorToQColor(cell.bg);
            QColor fgColor = vtermColorToQColor(cell.fg);
            if (cell.attrs.reverse)
                std::swap(bgColor, fgColor);

            painter.fillRect(x, y, m_cellWidth * cellWidth, m_cellHeight, bgColor);

            if (cell.chars[0] != 0) {
                painter.setPen(fgColor);
                const QFont &cellFont = fontForAttrs(cell.attrs.bold, cell.attrs.italic,
                                                     cell.attrs.underline, cell.attrs.strike);
                if (&cellFont != &m_font)
                    painter.setFont(cellFont);

                QString ch = QString::fromUcs4(&cell.chars[0], 1);
                painter.drawText(x, y + m_fontAscent, ch);

                if (&cellFont != &m_font)
                    painter.setFont(m_font);
            }

            col += cellWidth;
        }
    }
}

void SshSessionWidget::renderAllCells()
{
    if (!m_vterm || !m_vtermScreen)
        return;

    int rows, cols;
    vterm_get_size(m_vterm, &rows, &cols);
    VTermRect full = {0, rows, 0, cols};
    renderCells(full);
}

int SshSessionWidget::termCols() const
{
    return m_cellWidth > 0 ? qMax(1, width() / m_cellWidth) : 80;
}

int SshSessionWidget::termRows() const
{
    return m_cellHeight > 0 ? qMax(1, height() / m_cellHeight) : 24;
}

QColor SshSessionWidget::vtermColorToQColor(VTermColor color) const
{
    if (VTERM_COLOR_IS_INDEXED(&color)) {
        // Convert indexed to RGB via vterm
        vterm_screen_convert_color_to_rgb(m_vtermScreen, &color);
    }
    if (VTERM_COLOR_IS_DEFAULT_FG(&color)) {
        return QColor(204, 204, 204); // Light grey
    }
    if (VTERM_COLOR_IS_DEFAULT_BG(&color)) {
        return QColor(30, 30, 30); // Dark background
    }
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

void SshSessionWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    if (m_backBuffer.isNull() || !m_vterm || !m_vtermScreen) {
        painter.fillRect(rect(), QColor(30, 30, 30));
        return;
    }

    // Blit the back buffer (contains all cell content)
    painter.drawPixmap(0, 0, m_backBuffer);

    // Draw selection overlay on top
    if (m_hasSelection) {
        int rows, cols;
        vterm_get_size(m_vterm, &rows, &cols);

        VTermPos selMin = m_selStart, selMax = m_selEnd;
        if (selMin.row > selMax.row || (selMin.row == selMax.row && selMin.col > selMax.col))
            std::swap(selMin, selMax);

        int linearMin = selMin.row * cols + selMin.col;
        int linearMax = selMax.row * cols + selMax.col;

        for (int row = selMin.row; row <= selMax.row && row < rows; row++) {
            int c0 = (row == selMin.row) ? selMin.col : 0;
            int c1 = (row == selMax.row) ? selMax.col : cols - 1;

            for (int col = c0; col <= c1 && col < cols; col++) {
                int linearPos = row * cols + col;
                if (linearPos < linearMin || linearPos > linearMax)
                    continue;

                int x = col * m_cellWidth;
                int y = row * m_cellHeight;

                painter.fillRect(x, y, m_cellWidth, m_cellHeight, QColor(58, 114, 196));

                VTermScreenCell cell;
                VTermPos pos = {row, col};
                vterm_screen_get_cell(m_vtermScreen, pos, &cell);
                if (cell.chars[0] != 0) {
                    painter.setPen(QColor(255, 255, 255));
                    const QFont &cellFont = fontForAttrs(cell.attrs.bold, cell.attrs.italic,
                                                         cell.attrs.underline, cell.attrs.strike);
                    painter.setFont(cellFont);
                    QString ch = QString::fromUcs4(&cell.chars[0], 1);
                    painter.drawText(x, y + m_fontAscent, ch);
                }
            }
        }
        painter.setFont(m_font);
    }

    // Draw cursor on top
    if (m_cursorVisible && hasFocus()) {
        VTermPos cursorPos;
        vterm_state_get_cursorpos(vterm_obtain_state(m_vterm), &cursorPos);
        int cx = cursorPos.col * m_cellWidth;
        int cy = cursorPos.row * m_cellHeight;
        painter.fillRect(cx, cy, m_cellWidth, m_cellHeight, QColor(255, 255, 255, 128));
    }
}

void SshSessionWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_vterm)
        return;

    if (m_hasSelection) {
        m_hasSelection = false;
        update();
    }

    // Cmd+V (Mac) / Ctrl+V (Win) -> paste clipboard into terminal
    // On macOS: Qt::ControlModifier = Cmd key; on Windows: Qt::ControlModifier = Ctrl key
    bool isPaste = (event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_V;
    if (isPaste) {
        QString text = QGuiApplication::clipboard()->text();
        if (!text.isEmpty()) {
            QByteArray utf8 = text.toUtf8();
            for (char c : utf8)
                vterm_keyboard_unichar(m_vterm, static_cast<uint32_t>(c), VTERM_MOD_NONE);
        }
        return;
    }

    VTermModifier mod = VTERM_MOD_NONE;
    auto qtMod = event->modifiers();
    if (qtMod & Qt::ShiftModifier)   mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
    if (qtMod & Qt::AltModifier)     mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
    // On macOS: Qt::MetaModifier = physical Ctrl key, Qt::ControlModifier = Cmd key.
    // On Windows: Qt::ControlModifier = physical Ctrl key, Qt::MetaModifier = Win key.
#ifdef Q_OS_MACOS
    if (qtMod & Qt::MetaModifier)    mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
#else
    if (qtMod & Qt::ControlModifier) mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
#endif

    VTermKey vtKey = VTERM_KEY_NONE;

    switch (event->key()) {
    case Qt::Key_Return:    vtKey = VTERM_KEY_ENTER; break;
    case Qt::Key_Enter:     vtKey = VTERM_KEY_ENTER; break;
    case Qt::Key_Backspace: vtKey = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape:    vtKey = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Tab:       vtKey = VTERM_KEY_TAB; break;
    case Qt::Key_Up:        vtKey = VTERM_KEY_UP; break;
    case Qt::Key_Down:      vtKey = VTERM_KEY_DOWN; break;
    case Qt::Key_Left:      vtKey = VTERM_KEY_LEFT; break;
    case Qt::Key_Right:     vtKey = VTERM_KEY_RIGHT; break;
    case Qt::Key_Home:      vtKey = VTERM_KEY_HOME; break;
    case Qt::Key_End:       vtKey = VTERM_KEY_END; break;
    case Qt::Key_PageUp:    vtKey = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown:  vtKey = VTERM_KEY_PAGEDOWN; break;
    case Qt::Key_Insert:    vtKey = VTERM_KEY_INS; break;
    case Qt::Key_Delete:    vtKey = VTERM_KEY_DEL; break;
    case Qt::Key_F1:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1)); break;
    case Qt::Key_F2:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(2)); break;
    case Qt::Key_F3:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(3)); break;
    case Qt::Key_F4:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(4)); break;
    case Qt::Key_F5:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)); break;
    case Qt::Key_F6:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(6)); break;
    case Qt::Key_F7:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(7)); break;
    case Qt::Key_F8:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(8)); break;
    case Qt::Key_F9:  vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(9)); break;
    case Qt::Key_F10: vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)); break;
    case Qt::Key_F11: vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)); break;
    case Qt::Key_F12: vtKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)); break;
    default: break;
    }

    if (vtKey != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_vterm, vtKey, mod);
    } else {
        QString text = event->text();
#ifdef Q_OS_MACOS
        // On macOS, Meta+key (physical Ctrl) produces no text — synthesize the letter
        if (text.isEmpty() && (mod & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
            uint32_t letter = 'a' + (event->key() - Qt::Key_A);
            vterm_keyboard_unichar(m_vterm, letter, mod);
            return;
        }
#endif
        if (!text.isEmpty()) {
            uint32_t codepoint = text.at(0).unicode();
            vterm_keyboard_unichar(m_vterm, codepoint, mod);
        }
    }
}

void SshSessionWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_reconnectOverlay && m_reconnectOverlay->isVisible())
        m_reconnectOverlay->setGeometry(rect());

    int newCols = termCols();
    int newRows = termRows();

    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;

        if (m_vterm) {
            vterm_set_size(m_vterm, m_rows, m_cols);
        }
        emit resizeRequested(m_cols, m_rows);
    }

    // Recreate back buffer at new size and repaint all cells
    m_backBuffer = QPixmap(width(), height());
    m_backBuffer.fill(QColor(30, 30, 30));
    renderAllCells();
}

VTermPos SshSessionWidget::pixelToCell(const QPoint &pos) const
{
    VTermPos cell;
    cell.col = m_cellWidth > 0 ? qBound(0, pos.x() / m_cellWidth, m_cols - 1) : 0;
    cell.row = m_cellHeight > 0 ? qBound(0, pos.y() / m_cellHeight, m_rows - 1) : 0;
    return cell;
}

void SshSessionWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_selStart = pixelToCell(event->pos());
        m_selEnd = m_selStart;
        m_selecting = true;
        m_hasSelection = false;
        update();
    }
}

void SshSessionWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_selecting) {
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart.row != m_selEnd.row || m_selStart.col != m_selEnd.col);
        update();
    }
}

void SshSessionWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart.row != m_selEnd.row || m_selStart.col != m_selEnd.col);
        if (m_hasSelection) {
            QString text = selectedText();
            if (!text.isEmpty())
                QGuiApplication::clipboard()->setText(text);
        }
    }
}

QString SshSessionWidget::selectedText() const
{
    if (!m_vtermScreen || !m_hasSelection)
        return {};

    // Normalize so start <= end
    VTermPos start = m_selStart, end = m_selEnd;
    if (start.row > end.row || (start.row == end.row && start.col > end.col))
        std::swap(start, end);

    QString result;
    for (int row = start.row; row <= end.row; row++) {
        int c0 = (row == start.row) ? start.col : 0;
        int c1 = (row == end.row) ? end.col : m_cols - 1;
        for (int col = c0; col <= c1; col++) {
            VTermScreenCell cell;
            VTermPos pos = {row, col};
            vterm_screen_get_cell(m_vtermScreen, pos, &cell);
            if (cell.chars[0] != 0)
                result += QString::fromUcs4(&cell.chars[0], 1);
            else
                result += ' ';
        }
        // Trim trailing spaces on each line and add newline between rows
        if (row < end.row) {
            while (result.endsWith(' '))
                result.chop(1);
            result += '\n';
        }
    }
    return result;
}

// Claim all key events so Qt's shortcut system doesn't intercept
// terminal keys like Ctrl+C, Ctrl+Z, Ctrl+A, etc.
bool SshSessionWidget::event(QEvent *ev)
{
    if (ev->type() == QEvent::ShortcutOverride) {
        ev->accept();
        return true;
    }
#ifdef Q_OS_MACOS
    // On macOS, physical Ctrl+key arrives as Meta+key.  Some of these
    // (e.g. Meta+Z, Meta+X) may be swallowed by the QPA layer before
    // keyPressEvent.  Intercept them here and forward to keyPressEvent.
    if (ev->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if ((ke->modifiers() & Qt::MetaModifier) &&
            ke->key() >= Qt::Key_A && ke->key() <= Qt::Key_Z)
        {
            keyPressEvent(ke);
            return true;
        }
    }
#endif
    return QWidget::event(ev);
}


// Prevent Qt from consuming Tab for focus navigation
bool SshSessionWidget::focusNextPrevChild(bool /*next*/)
{
    return false;
}

void SshSessionWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    m_cursorVisible = true;
    m_blinkTimer->start();
    update();
}

void SshSessionWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    m_cursorVisible = true;
    m_blinkTimer->stop();
    update();
}

void SshSessionWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        for (const QChar &ch : event->commitString()) {
            vterm_keyboard_unichar(m_vterm, ch.unicode(), VTERM_MOD_NONE);
        }
    }
    event->accept();
}

QVariant SshSessionWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    return QWidget::inputMethodQuery(query);
}
