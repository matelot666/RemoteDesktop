#include "RdpSessionWidget.h"
#include "core/rdp/RdpClient.h"

#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QTimer>
#include <QDebug>
#include <cmath>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include "app/Application.h"
#include "core/config/ConfigManager.h"

#include <freerdp/input.h>

RdpSessionWidget::RdpSessionWidget(const ConnectionEntry &entry,
                                   const QString &username, const QString &password,
                                   QWidget *parent)
    : QWidget(parent), m_entry(entry)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_session = new RdpSession(entry, username, password);
    m_workerThread = new QThread(this);
    m_session->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_session, &RdpSession::start);
    connect(m_session, &RdpSession::connected, this, [this]() {
        m_renderTimer->start();
        emit sessionConnected();
    });
    connect(m_session, &RdpSession::disconnected, this, [this]() {
        m_renderTimer->stop();
        emit sessionDisconnected();
    });
    connect(m_session, &RdpSession::errorOccurred, this, &RdpSessionWidget::sessionError);
    connect(m_session, &RdpSession::reconnectStatus, this, [this](const QString &msg) {
        if (msg.isEmpty()) {
            m_reconnectOverlay->hide();
            m_renderTimer->start();
        } else {
            m_renderTimer->stop();
            m_reconnectOverlay->setText(msg);
            m_reconnectOverlay->setGeometry(rect());
            m_reconnectOverlay->show();
            m_reconnectOverlay->raise();
        }
    });

    // Reconnect overlay (hidden by default)
    m_reconnectOverlay = new QLabel(this);
    m_reconnectOverlay->setAlignment(Qt::AlignCenter);
    m_reconnectOverlay->setWordWrap(true);
    m_reconnectOverlay->setStyleSheet(
        QStringLiteral("QLabel { background-color: rgba(0, 0, 0, 180); color: white; "
                        "font-size: 16px; padding: 40px; }"));
    m_reconnectOverlay->hide();

    // Poll the session framebuffer at ~60fps instead of reacting to per-rect signals.
    // This avoids signal queue flooding when FreeRDP fires many EndPaint callbacks
    // (e.g. initial screen paint with dozens of tiles).
    m_renderTimer = new QTimer(this);
    m_renderTimer->setTimerType(Qt::PreciseTimer);
    m_renderTimer->setInterval(16);
    connect(m_renderTimer, &QTimer::timeout, this, &RdpSessionWidget::renderFrame);

    connect(m_workerThread, &QThread::finished, m_session, &QObject::deleteLater);

    // Clipboard: remote → local
    if (m_entry.enableClipboard) {
        connect(m_session, &RdpSession::remoteClipboardChanged, this, [this](const QString &text) {
            QClipboard *clipboard = QGuiApplication::clipboard();
            if (clipboard) {
                m_suppressClipboardEcho = true;
                clipboard->setText(text);
#ifdef Q_OS_MACOS
                // Update poll signature so the timer doesn't re-announce this change
                m_lastClipboardSignature = {QStringLiteral("text:") + text.left(256)};
#endif
            }
        }, Qt::QueuedConnection);

        connect(m_session, &RdpSession::remoteFilesReceived, this,
                [this](const QStringList &paths, const QString &originalText) {
            QClipboard *clipboard = QGuiApplication::clipboard();
            if (!clipboard)
                return;
            auto *mimeData = new QMimeData();
            QList<QUrl> urls;
            for (const QString &path : paths)
                urls.append(QUrl::fromLocalFile(path));
            mimeData->setUrls(urls);
            // Use original remote text if available (e.g. UNC paths), otherwise filenames
            if (!originalText.isEmpty()) {
                mimeData->setText(originalText);
            } else {
                QStringList names;
                for (const QString &path : paths)
                    names.append(QFileInfo(path).fileName());
                mimeData->setText(names.join(QStringLiteral("\n")));
            }
            m_suppressClipboardEcho = true;
            clipboard->setMimeData(mimeData);
#ifdef Q_OS_MACOS
            // Update poll signature so the timer doesn't re-announce this change
            QStringList sig;
            for (const QUrl &url : urls)
                sig.append(url.toString());
            m_lastClipboardSignature = sig;
#endif
        }, Qt::QueuedConnection);

        // Clipboard: local → remote
        // Detect clipboard changes and re-announce formats to the server.
        auto clipboardChanged = [this]() {
            if (!m_session)
                return;
            // Skip if this change was caused by us receiving data from the remote
            if (m_suppressClipboardEcho) {
                m_suppressClipboardEcho = false;
                return;
            }
            QClipboard *cb = QGuiApplication::clipboard();
            if (!cb)
                return;
            const QMimeData *mimeData = cb->mimeData();
            if (!mimeData)
                return;

            if (mimeData->hasUrls()) {
                QStringList filePaths;
                for (const QUrl &url : mimeData->urls()) {
                    if (url.isLocalFile())
                        filePaths.append(url.toLocalFile());
                }
                if (!filePaths.isEmpty()) {
                    QMetaObject::invokeMethod(m_session, "announceLocalFiles",
                                              Qt::QueuedConnection,
                                              Q_ARG(QStringList, filePaths));
                    return;
                }
            }
            QMetaObject::invokeMethod(m_session, "sendLocalClipboardToServer",
                                      Qt::QueuedConnection);
        };

        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard) {
            connect(clipboard, &QClipboard::dataChanged, this, clipboardChanged);
        }

#ifdef Q_OS_MACOS
        // macOS doesn't reliably fire QClipboard::dataChanged for external apps
        // (Finder, etc.). Poll the clipboard to detect changes.
        auto *pollTimer = new QTimer(this);
        connect(pollTimer, &QTimer::timeout, this, [this, clipboardChanged]() {
            if (!m_session) return;
            QClipboard *cb = QGuiApplication::clipboard();
            if (!cb) return;
            const QMimeData *md = cb->mimeData();
            if (!md) return;

            // Build a signature of the current clipboard content
            QStringList sig;
            if (md->hasUrls()) {
                for (const QUrl &url : md->urls())
                    sig.append(url.toString());
            } else if (md->hasText()) {
                sig.append(QStringLiteral("text:") + md->text().left(256));
            }

            if (sig == m_lastClipboardSignature)
                return;
            m_lastClipboardSignature = sig;

            clipboardChanged();
        });
        pollTimer->start(500);
#endif
    }
}

RdpSessionWidget::~RdpSessionWidget()
{
    disconnectSession();
}

void RdpSessionWidget::connectSession()
{
    // Defer start until after Qt has performed the layout pass,
    // so size() returns the actual widget dimensions.
    QTimer::singleShot(0, this, [this]() {
        if (!m_session)  // disconnectSession() was called before this fired
            return;
        QSize sz = size();
        qDebug() << "RdpSessionWidget: widget size at connect =" << sz;
        if (sz.width() > 0 && sz.height() > 0) {
            m_session->setDesktopSize(sz.width(), sz.height());
        }
        // Apply global RDP options from config
        auto *cfg = Application::instance()->config();
        RdpOptions opts;
        opts.gfxPipeline = cfg->rdpGfxPipeline();
        opts.h264 = cfg->rdpH264();
        opts.remoteFx = cfg->rdpRemoteFx();
        opts.fontSmoothing = cfg->rdpFontSmoothing();
        opts.verboseLog = cfg->rdpVerboseLog();
        m_session->setRdpOptions(opts);
        m_workerThread->start();
    });
}

void RdpSessionWidget::disconnectSession()
{
    if (!m_session)
        return;

    m_renderTimer->stop();

    // Signal the session to stop (sets m_stopRequested + m_running = false).
    // This interrupts both eventLoop() and prevents start() from proceeding.
    m_session->stop();

    if (m_workerThread->isRunning()) {
        m_workerThread->quit();
        // Wait long enough for freerdp_connect + freerdp_disconnect to complete.
        // freerdp_connect can take several seconds (TLS, NLA handshakes).
        if (!m_workerThread->wait(30000)) {
            qWarning() << "RDP worker thread didn't finish in 30s, terminating";
            m_workerThread->terminate();
            m_workerThread->wait();
        }
    }

    m_session = nullptr; // Will be deleted by QThread::finished → deleteLater
}

void RdpSessionWidget::renderFrame()
{
    if (!m_session)
        return;

    // Atomically grab the accumulated dirty region and copy it in one mutex lock.
    // This replaces per-EndPaint signal delivery — one poll per display frame
    // instead of dozens of queued signals.
    QRect dirty = m_session->takeDirtyRegion(m_displayBuffer);
    if (dirty.isEmpty())
        return;

    // Recalculate scale (handles first frame and desktop resize)
    if (!m_displayBuffer.isNull()) {
        double scaleX = static_cast<double>(width()) / m_displayBuffer.width();
        double scaleY = static_cast<double>(height()) / m_displayBuffer.height();
        m_scale = qMin(scaleX, scaleY);

        int scaledW = static_cast<int>(m_displayBuffer.width() * m_scale);
        int scaledH = static_cast<int>(m_displayBuffer.height() * m_scale);
        m_offset = QPoint((width() - scaledW) / 2, (height() - scaledH) / 2);
    }

    // Schedule repaint for only the affected region (scaled to widget coordinates)
    QRect widgetDirty(
        static_cast<int>(dirty.x() * m_scale) + m_offset.x(),
        static_cast<int>(dirty.y() * m_scale) + m_offset.y(),
        static_cast<int>(std::ceil(dirty.width() * m_scale)) + 2,
        static_cast<int>(std::ceil(dirty.height() * m_scale)) + 2
    );
    update(widgetDirty);
}

void RdpSessionWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_displayBuffer.isNull()) {
        painter.fillRect(event->rect(), Qt::black);
        return;
    }

    int scaledW = static_cast<int>(m_displayBuffer.width() * m_scale);
    int scaledH = static_cast<int>(m_displayBuffer.height() * m_scale);
    QRect destRect(m_offset, QSize(scaledW, scaledH));

    // Fill black only in letterbox margins that intersect the update region
    QRegion marginRegion = QRegion(event->rect()) - destRect;
    for (const QRect &r : marginRegion)
        painter.fillRect(r, Qt::black);

    // Draw only the portion of the source image that intersects the update rect
    QRect imageArea = destRect.intersected(event->rect());
    if (imageArea.isEmpty())
        return;

    double invScale = 1.0 / m_scale;
    QRect sourceRect(
        static_cast<int>((imageArea.x() - m_offset.x()) * invScale),
        static_cast<int>((imageArea.y() - m_offset.y()) * invScale),
        static_cast<int>(std::ceil(imageArea.width() * invScale)) + 1,
        static_cast<int>(std::ceil(imageArea.height() * invScale)) + 1
    );
    sourceRect = sourceRect.intersected(m_displayBuffer.rect());

    painter.drawImage(imageArea, m_displayBuffer, sourceRect);
}

QPoint RdpSessionWidget::mapToDesktop(const QPoint &widgetPos) const
{
    if (m_displayBuffer.isNull() || m_scale == 0.0)
        return {};

    double x = (widgetPos.x() - m_offset.x()) / m_scale;
    double y = (widgetPos.y() - m_offset.y()) / m_scale;

    return QPoint(qBound(0, static_cast<int>(x), m_displayBuffer.width() - 1),
                  qBound(0, static_cast<int>(y), m_displayBuffer.height() - 1));
}

void RdpSessionWidget::mousePressEvent(QMouseEvent *event)
{
    QPoint deskPos = mapToDesktop(event->pos());
    uint16_t flags = 0;
    switch (event->button()) {
    case Qt::LeftButton:   flags = PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN; break;
    case Qt::RightButton:  flags = PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN; break;
    case Qt::MiddleButton: flags = PTR_FLAGS_BUTTON3 | PTR_FLAGS_DOWN; break;
    default: break;
    }
    if (flags && m_session)
        QMetaObject::invokeMethod(m_session, "sendMouseEvent", Qt::QueuedConnection,
                                  Q_ARG(uint16_t, flags), Q_ARG(int, deskPos.x()), Q_ARG(int, deskPos.y()));
}

void RdpSessionWidget::mouseReleaseEvent(QMouseEvent *event)
{
    QPoint deskPos = mapToDesktop(event->pos());
    uint16_t flags = 0;
    switch (event->button()) {
    case Qt::LeftButton:   flags = PTR_FLAGS_BUTTON1; break;
    case Qt::RightButton:  flags = PTR_FLAGS_BUTTON2; break;
    case Qt::MiddleButton: flags = PTR_FLAGS_BUTTON3; break;
    default: break;
    }
    if (flags && m_session)
        QMetaObject::invokeMethod(m_session, "sendMouseEvent", Qt::QueuedConnection,
                                  Q_ARG(uint16_t, flags), Q_ARG(int, deskPos.x()), Q_ARG(int, deskPos.y()));
}

void RdpSessionWidget::mouseMoveEvent(QMouseEvent *event)
{
    QPoint deskPos = mapToDesktop(event->pos());
    if (m_session)
        QMetaObject::invokeMethod(m_session, "sendMouseEvent", Qt::QueuedConnection,
                                  Q_ARG(uint16_t, static_cast<uint16_t>(PTR_FLAGS_MOVE)),
                                  Q_ARG(int, deskPos.x()), Q_ARG(int, deskPos.y()));
}

void RdpSessionWidget::wheelEvent(QWheelEvent *event)
{
    QPoint deskPos = mapToDesktop(event->position().toPoint());
    int delta = event->angleDelta().y();
    uint16_t flags = PTR_FLAGS_WHEEL;
    if (delta < 0) {
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        delta = -delta;
    }
    flags |= (delta & 0x01FF);
    if (m_session)
        QMetaObject::invokeMethod(m_session, "sendMouseEvent", Qt::QueuedConnection,
                                  Q_ARG(uint16_t, flags), Q_ARG(int, deskPos.x()), Q_ARG(int, deskPos.y()));
}

// Prevent Qt from consuming Tab for focus navigation
bool RdpSessionWidget::focusNextPrevChild(bool /*next*/)
{
    return false;
}

// Helper: queue a keyboard event to the RDP session thread and track state
void RdpSessionWidget::sendKey(bool down, uint16_t scancode, bool extended)
{
    if (!m_session)
        return;

    uint32_t key = scancode | (extended ? 0x10000u : 0u);
    if (down)
        m_pressedKeys.insert(key);
    else
        m_pressedKeys.remove(key);

    QMetaObject::invokeMethod(m_session, "sendKeyboardEvent", Qt::QueuedConnection,
                              Q_ARG(bool, down), Q_ARG(uint16_t, scancode), Q_ARG(bool, extended));
}

// Release all keys we think are held — prevents stuck modifiers on focus loss
void RdpSessionWidget::releaseAllKeys()
{
    for (uint32_t key : m_pressedKeys) {
        uint16_t sc = static_cast<uint16_t>(key & 0xFFFF);
        bool ext = (key & 0x10000u) != 0;
        if (m_session)
            QMetaObject::invokeMethod(m_session, "sendKeyboardEvent", Qt::QueuedConnection,
                                      Q_ARG(bool, false), Q_ARG(uint16_t, sc), Q_ARG(bool, ext));
    }
    m_pressedKeys.clear();
#ifdef Q_OS_MACOS
    m_cmdHeld = false;
    m_ctrlSentForEdit = false;
#endif
}

void RdpSessionWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    releaseAllKeys();
}

#ifdef Q_OS_MACOS
// macOS native virtual keycodes for Cmd keys
static constexpr int kVK_Command = 0x37;
static constexpr int kVK_RightCommand = 0x36;

// macOS native virtual keycodes for edit keys (C, X, V, A, Z)
static bool isEditKey(int nativeKey)
{
    return nativeKey == 0x08  // kVK_ANSI_C
        || nativeKey == 0x07  // kVK_ANSI_X
        || nativeKey == 0x09  // kVK_ANSI_V
        || nativeKey == 0x00  // kVK_ANSI_A
        || nativeKey == 0x06; // kVK_ANSI_Z
}
#endif

void RdpSessionWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_session)
        return;

    // Filter Qt auto-repeat: RDP server handles key repeat internally.
    // Without this, the GFX pipeline's faster event loop causes auto-repeat
    // key-downs to flood the server at an excessive rate.
    if (event->isAutoRepeat())
        return;

    event->accept(); // Prevent Qt from handling (e.g. Tab focus navigation)

#ifdef Q_OS_MACOS
    // --- macOS: use native virtual keycodes + lookup table ---
    int nativeKey = event->nativeVirtualKey();

    // Handle Cmd key press
    if (nativeKey == kVK_Command || nativeKey == kVK_RightCommand) {
        if (m_entry.winKeyCopyPaste) {
            m_cmdHeld = true;
            return;
        }
        if (m_entry.winKeyPassthrough) {
            RdpScancode sc = macKeyToRdpScancode(nativeKey);
            if (sc.code != 0)
                sendKey(true, sc.code, sc.extended);
            return;
        }
        return; // suppress
    }

    // Cmd held + edit key → send Ctrl+letter
    if (m_cmdHeld && m_entry.winKeyCopyPaste && isEditKey(nativeKey)) {
        RdpScancode sc = macKeyToRdpScancode(nativeKey);
        if (sc.code != 0) {
            sendKey(true, 0x1D, false); // Left Ctrl down
            sendKey(true, sc.code, sc.extended);
            m_ctrlSentForEdit = true;
        }
        return;
    }

    // Cmd held + non-edit key → flush as Win key if passthrough enabled
    if (m_cmdHeld && !isEditKey(nativeKey)) {
        if (m_entry.winKeyPassthrough)
            sendKey(true, 0x5B, true); // Win key down
        m_cmdHeld = false;
    }

    RdpScancode sc = macKeyToRdpScancode(nativeKey);
    if (sc.code != 0)
        sendKey(true, sc.code, sc.extended);

#else
    // --- Windows/Linux: use native scan code directly ---
    uint16_t sc = static_cast<uint16_t>(event->nativeScanCode());
    if (sc == 0)
        return;
    bool ext = isRdpExtendedKey(event->key());
    sendKey(true, sc, ext);
#endif
}

void RdpSessionWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
        return;

    if (!m_session)
        return;

    event->accept();

#ifdef Q_OS_MACOS
    int nativeKey = event->nativeVirtualKey();

    // Handle Cmd key release
    if (nativeKey == kVK_Command || nativeKey == kVK_RightCommand) {
        // Always release synthetic Ctrl if we sent it
        if (m_ctrlSentForEdit) {
            sendKey(false, 0x1D, false); // Left Ctrl up
            m_ctrlSentForEdit = false;
        }
        if (m_cmdHeld) {
            // Bare Cmd tap → send Win key tap if passthrough enabled
            if (m_entry.winKeyPassthrough) {
                sendKey(true, 0x5B, true);
                sendKey(false, 0x5B, true);
            }
            m_cmdHeld = false;
            return;
        }
        if (m_entry.winKeyPassthrough) {
            RdpScancode sc = macKeyToRdpScancode(nativeKey);
            if (sc.code != 0)
                sendKey(false, sc.code, sc.extended);
            return;
        }
        return; // suppress
    }

    // Edit key release while Cmd was held: release the letter, then Ctrl
    if (m_ctrlSentForEdit && isEditKey(nativeKey)) {
        RdpScancode sc = macKeyToRdpScancode(nativeKey);
        if (sc.code != 0)
            sendKey(false, sc.code, sc.extended);
        sendKey(false, 0x1D, false); // Left Ctrl up
        m_ctrlSentForEdit = false;
        return;
    }

    // Normal key release
    RdpScancode sc = macKeyToRdpScancode(nativeKey);
    if (sc.code != 0)
        sendKey(false, sc.code, sc.extended);

#else
    // --- Windows/Linux: use native scan code directly ---
    uint16_t sc = static_cast<uint16_t>(event->nativeScanCode());
    if (sc == 0)
        return;
    bool ext = isRdpExtendedKey(event->key());
    sendKey(false, sc, ext);
#endif
}

void RdpSessionWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_reconnectOverlay && m_reconnectOverlay->isVisible())
        m_reconnectOverlay->setGeometry(rect());
    if (!m_displayBuffer.isNull()) {
        double scaleX = static_cast<double>(width()) / m_displayBuffer.width();
        double scaleY = static_cast<double>(height()) / m_displayBuffer.height();
        m_scale = qMin(scaleX, scaleY);

        int scaledW = static_cast<int>(m_displayBuffer.width() * m_scale);
        int scaledH = static_cast<int>(m_displayBuffer.height() * m_scale);
        m_offset = QPoint((width() - scaledW) / 2, (height() - scaledH) / 2);
        update();
    }
}
