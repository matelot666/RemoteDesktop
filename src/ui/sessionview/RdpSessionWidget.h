#pragma once

#include <QWidget>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <QSet>
#include "core/rdp/RdpSession.h"
#include "core/ConnectionEntry.h"

class RdpSessionWidget : public QWidget {
    Q_OBJECT
public:
    explicit RdpSessionWidget(const ConnectionEntry &entry,
                              const QString &username, const QString &password,
                              QWidget *parent = nullptr);
    ~RdpSessionWidget() override;

    void connectSession();
    void disconnectSession();
    qint64 connectionId() const { return m_entry.id; }

signals:
    void sessionConnected();
    void sessionDisconnected();
    void sessionError(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void renderFrame();
    QPoint mapToDesktop(const QPoint &widgetPos) const;
    uint16_t mouseButtonFlags(Qt::MouseButtons buttons, bool press) const;
    void sendKey(bool down, uint16_t scancode, bool extended);
    void releaseAllKeys();

    ConnectionEntry m_entry;
    RdpSession *m_session = nullptr;
    QThread *m_workerThread = nullptr;
    QTimer *m_renderTimer = nullptr;
    QImage m_displayBuffer;
    double m_scale = 1.0;
    QPoint m_offset;
    QSet<uint32_t> m_pressedKeys; // Track keys sent as down (scancode | extended<<16)
#ifdef Q_OS_MACOS
    bool m_cmdHeld = false;        // Tracks whether Cmd is currently held (for Cmd→Ctrl translation)
    bool m_ctrlSentForEdit = false; // Tracks whether synthetic Ctrl-down was sent for edit key
    QStringList m_lastClipboardSignature; // For clipboard polling (dataChanged unreliable on macOS)
#endif
};
