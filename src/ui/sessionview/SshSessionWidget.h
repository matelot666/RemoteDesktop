#pragma once

#include <QWidget>
#include <QThread>
#include <QFont>
#include <QLabel>
#include <QTimer>
#include <QPixmap>
#ifdef _WIN32
// Windows SDK rpcndr.h defines 'small' as a macro for 'char',
// which collides with libvterm's VTermScreenCellAttrs::small bitfield.
#pragma push_macro("small")
#undef small
#endif
#include <vterm.h>
#ifdef _WIN32
#pragma pop_macro("small")
#endif
#include "core/ssh/SshSession.h"
#include "core/ConnectionEntry.h"

class SshSessionWidget : public QWidget {
    Q_OBJECT
public:
    explicit SshSessionWidget(const ConnectionEntry &entry,
                              const QString &username, const QString &password,
                              const QString &privateKey,
                              QWidget *parent = nullptr);
    ~SshSessionWidget() override;

    void connectSession();
    void disconnectSession();
    qint64 connectionId() const { return m_entry.id; }

signals:
    void sessionConnected();
    void sessionDisconnected();
    void sessionError(const QString &message);
    void writeRequested(const QByteArray &data);
    void resizeRequested(int cols, int rows);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    void onDataReceived(const QByteArray &data);
    void setupVterm();
    void destroyVterm();
    QColor vtermColorToQColor(VTermColor color) const;
    void calculateCellSize();
    void initFontVariants();
    const QFont &fontForAttrs(unsigned bold, unsigned italic, unsigned underline, unsigned strike) const;
    void renderCells(const VTermRect &rect);
    void renderAllCells();
    int termCols() const;
    int termRows() const;

    static void vtermOutputCallback(const char *s, size_t len, void *user);

    VTermScreenCallbacks m_vtermCallbacks = {};
    ConnectionEntry m_entry;
    SshSession *m_session = nullptr;
    QThread *m_workerThread = nullptr;

    VTerm *m_vterm = nullptr;
    VTermScreen *m_vtermScreen = nullptr;

    // Back buffer — only damaged cells are repainted into this pixmap
    QPixmap m_backBuffer;

    // Font variants (cached to avoid per-cell QFont construction)
    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    QFont m_fontUnderline;
    QFont m_fontBoldUnderline;
    QFont m_fontStrike;

    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_fontAscent = 0;
    int m_cols = 80;
    int m_rows = 24;

    bool m_cursorVisible = true;
    QTimer *m_blinkTimer = nullptr;

    // Text selection (terminal coordinates)
    VTermPos pixelToCell(const QPoint &pos) const;
    QString selectedText() const;
    QLabel *m_reconnectOverlay = nullptr;
    bool m_selecting = false;
    bool m_hasSelection = false;
    VTermPos m_selStart = {0, 0};
    VTermPos m_selEnd = {0, 0};
};
