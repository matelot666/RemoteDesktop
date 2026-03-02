#include "SessionTabWidget.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QSvgRenderer>

// Custom widget that paints the SVG centered at 30% opacity
class LogoWidget : public QWidget {
public:
    explicit LogoWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        m_renderer = new QSvgRenderer(QStringLiteral(":/icons/nobs-rdp-logo.svg"), this);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_renderer->isValid())
            return;

        QPainter p(this);
        p.setOpacity(0.3);

        // SVG native size is 1650x300; render 512px wide, preserving aspect ratio
        constexpr int logoW = 512;
        constexpr int logoH = static_cast<int>(512.0 * 300.0 / 1650.0);  // ~93px
        int x = (width() - logoW) / 2;
        int y = (height() - logoH) / 2;
        m_renderer->render(&p, QRectF(x, y, logoW, logoH));
    }

private:
    QSvgRenderer *m_renderer;
};

SessionTabWidget::SessionTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);

    m_emptyState = new LogoWidget(this);
    m_emptyState->setAttribute(Qt::WA_TransparentForMouseEvents);

    connect(this, &QTabWidget::tabCloseRequested, this, &SessionTabWidget::onTabCloseRequested);
}

int SessionTabWidget::addSessionTab(QWidget *widget, const QString &label, qint64 connectionId)
{
    m_sessions.insert(connectionId, widget);
    int idx = addTab(widget, label);
    setCurrentIndex(idx);
    updateEmptyState();
    emit sessionCountChanged(m_sessions.size());
    return idx;
}

QWidget *SessionTabWidget::sessionForConnection(qint64 connectionId) const
{
    return m_sessions.value(connectionId, nullptr);
}

void SessionTabWidget::removeSessionTab(qint64 connectionId)
{
    auto *widget = m_sessions.value(connectionId, nullptr);
    if (widget) {
        int idx = indexOf(widget);
        if (idx >= 0)
            removeTab(idx);
        m_sessions.remove(connectionId);
        widget->deleteLater();
        updateEmptyState();
        emit sessionCountChanged(m_sessions.size());
    }
}

int SessionTabWidget::activeSessionCount() const
{
    return m_sessions.size();
}

void SessionTabWidget::onTabCloseRequested(int index)
{
    QWidget *widget = this->widget(index);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it.value() == widget) {
            emit tabCloseRequested(it.key());
            return;
        }
    }
}

void SessionTabWidget::updateEmptyState()
{
    bool empty = m_sessions.isEmpty();
    m_emptyState->setVisible(empty);
    if (empty) {
        m_emptyState->setGeometry(rect());
        m_emptyState->raise();
    }
}

void SessionTabWidget::showEvent(QShowEvent *event)
{
    QTabWidget::showEvent(event);
    if (m_emptyState->isVisible())
        m_emptyState->setGeometry(rect());
}

void SessionTabWidget::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    if (m_emptyState->isVisible())
        m_emptyState->setGeometry(rect());
}
