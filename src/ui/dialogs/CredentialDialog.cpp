#include "CredentialDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFileDialog>
#include <QDir>

CredentialDialog::CredentialDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Credential"));
    setMinimumWidth(450);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_nameEdit = new QLineEdit;
    form->addRow(QStringLiteral("Name:"), m_nameEdit);

    m_typeCombo = new QComboBox;
    m_typeCombo->addItem(QStringLiteral("Username / Password"),
                         static_cast<int>(CredentialType::UsernamePassword));
    m_typeCombo->addItem(QStringLiteral("Private Key"),
                         static_cast<int>(CredentialType::PrivateKey));
    connect(m_typeCombo, &QComboBox::currentIndexChanged,
            this, &CredentialDialog::onTypeChanged);
    form->addRow(QStringLiteral("Type:"), m_typeCombo);

    m_usernameEdit = new QLineEdit;
    form->addRow(QStringLiteral("Username:"), m_usernameEdit);

    layout->addLayout(form);

    m_authStack = new QStackedWidget;

    // Page 0: password
    auto *passwordPage = new QWidget;
    auto *pwLayout = new QFormLayout(passwordPage);
    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    pwLayout->addRow(QStringLiteral("Password:"), m_passwordEdit);
    m_authStack->addWidget(passwordPage);

    // Page 1: private key file
    auto *keyPage = new QWidget;
    auto *keyLayout = new QFormLayout(keyPage);

    auto *keyFileRow = new QHBoxLayout;
    m_keyFileEdit = new QLineEdit;
    m_keyFileEdit->setPlaceholderText(QStringLiteral("~/.ssh/id_rsa"));
    keyFileRow->addWidget(m_keyFileEdit);
    auto *browseBtn = new QPushButton(QStringLiteral("Browse..."));
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString startDir = QDir::homePath() + QStringLiteral("/.ssh");
        QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Select Private Key"),
                                                     startDir);
        if (!file.isEmpty())
            m_keyFileEdit->setText(file);
    });
    keyFileRow->addWidget(browseBtn);
    keyLayout->addRow(QStringLiteral("Key File:"), keyFileRow);

    m_keyPassphraseEdit = new QLineEdit;
    m_keyPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_keyPassphraseEdit->setPlaceholderText(QStringLiteral("(leave empty if none)"));
    keyLayout->addRow(QStringLiteral("Passphrase:"), m_keyPassphraseEdit);

    m_authStack->addWidget(keyPage);

    layout->addWidget(m_authStack);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void CredentialDialog::setCredential(const Credential &cred)
{
    m_credential = cred;
    m_nameEdit->setText(cred.name);
    m_typeCombo->setCurrentIndex(static_cast<int>(cred.type));
    m_usernameEdit->setText(cred.username);
    m_passwordEdit->setText(cred.password);
    m_keyFileEdit->setText(cred.privateKey);     // Now stores file path
    m_keyPassphraseEdit->setText(cred.password);  // Passphrase reuses password field
}

Credential CredentialDialog::credential() const
{
    Credential c = m_credential;
    c.name = m_nameEdit->text().trimmed();
    c.type = static_cast<CredentialType>(m_typeCombo->currentData().toInt());
    c.username = m_usernameEdit->text();

    if (c.type == CredentialType::UsernamePassword) {
        c.password = m_passwordEdit->text();
        c.privateKey.clear();
    } else {
        c.privateKey = m_keyFileEdit->text().trimmed();  // File path
        c.password = m_keyPassphraseEdit->text();         // Passphrase
    }
    return c;
}

void CredentialDialog::onTypeChanged(int index)
{
    m_authStack->setCurrentIndex(index);
}
