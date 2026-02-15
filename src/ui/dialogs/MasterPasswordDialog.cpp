#include "MasterPasswordDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

MasterPasswordDialog::MasterPasswordDialog(Mode mode, QWidget *parent)
    : QDialog(parent), m_mode(mode)
{
    setWindowTitle(mode == Setup
        ? QStringLiteral("Set Master Password")
        : QStringLiteral("Unlock Vault"));
    setMinimumWidth(350);

    auto *layout = new QVBoxLayout(this);

    auto *infoLabel = new QLabel(mode == Setup
        ? QStringLiteral("Create a master password to encrypt your credentials.")
        : QStringLiteral("Enter your master password to unlock the credential vault."));
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto *form = new QFormLayout;

    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Password:"), m_passwordEdit);

    if (mode == Setup) {
        m_confirmEdit = new QLineEdit;
        m_confirmEdit->setEchoMode(QLineEdit::Password);
        form->addRow(QStringLiteral("Confirm:"), m_confirmEdit);
    }

    layout->addLayout(form);

    m_errorLabel = new QLabel;
    m_errorLabel->setStyleSheet(QStringLiteral("color: red;"));
    m_errorLabel->hide();
    layout->addWidget(m_errorLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &MasterPasswordDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    m_passwordEdit->setFocus();
}

QString MasterPasswordDialog::password() const
{
    return m_passwordEdit->text();
}

void MasterPasswordDialog::onAccept()
{
    if (m_passwordEdit->text().isEmpty()) {
        m_errorLabel->setText(QStringLiteral("Password cannot be empty."));
        m_errorLabel->show();
        return;
    }

    if (m_mode == Setup) {
        if (m_passwordEdit->text() != m_confirmEdit->text()) {
            m_errorLabel->setText(QStringLiteral("Passwords do not match."));
            m_errorLabel->show();
            return;
        }
        if (m_passwordEdit->text().length() < 8) {
            m_errorLabel->setText(QStringLiteral("Password must be at least 8 characters."));
            m_errorLabel->show();
            return;
        }
    }

    accept();
}
