#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include "core/ConnectionEntry.h"

class QuickConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit QuickConnectDialog(QWidget *parent = nullptr);

    ConnectionEntry connectionEntry() const;
    QString username() const { return m_resolvedUsername; }
    QString password() const { return m_resolvedPassword; }
    QString privateKey() const { return m_resolvedPrivateKey; }

    void accept() override;

private:
    void onProtocolChanged(int index);
    void onCredentialModeChanged(int index);

    // General
    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostnameEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;

    // Credential mode
    QComboBox *m_credentialModeCombo = nullptr;

    // Saved credential
    QWidget *m_savedCredentialWidget = nullptr;
    QComboBox *m_savedCredentialCombo = nullptr;

    // Ad-hoc credential
    QWidget *m_adHocCredentialWidget = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLabel *m_privateKeyLabel = nullptr;
    QLineEdit *m_privateKeyEdit = nullptr;
    QPushButton *m_browseKeyButton = nullptr;

    // RDP options
    QGroupBox *m_rdpOptionsGroup = nullptr;
    QComboBox *m_colorDepthCombo = nullptr;
    QComboBox *m_securityCombo = nullptr;
    QCheckBox *m_clipboardCheck = nullptr;
    QCheckBox *m_compressionCheck = nullptr;

    // Resolved credentials (cached on accept)
    QString m_resolvedUsername;
    QString m_resolvedPassword;
    QString m_resolvedPrivateKey;
};
