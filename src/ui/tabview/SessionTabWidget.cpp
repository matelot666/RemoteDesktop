#include "SessionTabWidget.h"

SessionTabWidget::SessionTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);

    connect(this, &QTabWidget::tabCloseRequested, this, &SessionTabWidget::onTabCloseRequested);
}

int SessionTabWidget::addSessionTab(QWidget *widget, const QString &label, qint64 connectionId)
{
    m_sessions.insert(connectionId, widget);
    int idx = addTab(widget, label);
    setCurrentIndex(idx);
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
