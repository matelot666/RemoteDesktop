#pragma once

#include <QDialog>

class RdpSession;

class SessionInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit SessionInfoDialog(RdpSession *session, QWidget *parent = nullptr);
};
