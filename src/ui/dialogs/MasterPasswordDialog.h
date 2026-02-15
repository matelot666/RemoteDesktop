#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>

class MasterPasswordDialog : public QDialog {
    Q_OBJECT
public:
    enum Mode { Unlock, Setup };

    explicit MasterPasswordDialog(Mode mode, QWidget *parent = nullptr);

    QString password() const;

private:
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_confirmEdit = nullptr;
    QLabel *m_errorLabel = nullptr;
    Mode m_mode;

    void onAccept();
};
