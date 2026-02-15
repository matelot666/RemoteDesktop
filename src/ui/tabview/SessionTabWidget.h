#pragma once

#include <QTabWidget>
#include <QMap>

class SessionTabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit SessionTabWidget(QWidget *parent = nullptr);

    int addSessionTab(QWidget *widget, const QString &label, qint64 connectionId);
    QWidget *sessionForConnection(qint64 connectionId) const;
    void removeSessionTab(qint64 connectionId);
    int activeSessionCount() const;

signals:
    void tabCloseRequested(qint64 connectionId);
    void sessionCountChanged(int count);

private:
    QMap<qint64, QWidget *> m_sessions;

    void onTabCloseRequested(int index);
};
