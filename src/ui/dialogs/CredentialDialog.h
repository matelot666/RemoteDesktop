#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include "core/Credential.h"

class CredentialDialog : public QDialog {
    Q_OBJECT
public:
    explicit CredentialDialog(QWidget *parent = nullptr);

    void setCredential(const Credential &cred);
    Credential credential() const;

private:
    void onTypeChanged(int index);

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_keyFileEdit = nullptr;
    QLineEdit *m_keyPassphraseEdit = nullptr;
    QStackedWidget *m_authStack = nullptr;
    Credential m_credential;
};
