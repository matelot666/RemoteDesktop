#include "CredentialManagerDialog.h"
#include "CredentialDialog.h"
#include "app/Application.h"
#include "core/userdb/UserDatabase.h"
#include "core/credentials/CredentialVault.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

CredentialManagerDialog::CredentialManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Manage Credentials"));
    setMinimumSize(450, 350);

    auto *layout = new QVBoxLayout(this);

    m_listWidget = new QListWidget;
    layout->addWidget(m_listWidget);

    auto *btnLayout = new QHBoxLayout;

    auto *addBtn = new QPushButton(QStringLiteral("Add..."));
    m_editBtn = new QPushButton(QStringLiteral("Edit..."));
    m_deleteBtn = new QPushButton(QStringLiteral("Delete"));
    auto *closeBtn = new QPushButton(QStringLiteral("Close"));

    m_editBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);

    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(m_editBtn);
    btnLayout->addWidget(m_deleteBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);

    connect(addBtn, &QPushButton::clicked, this, &CredentialManagerDialog::addCredential);
    connect(m_editBtn, &QPushButton::clicked, this, &CredentialManagerDialog::editCredential);
    connect(m_deleteBtn, &QPushButton::clicked, this, &CredentialManagerDialog::deleteCredential);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_listWidget, &QListWidget::currentRowChanged, this, [this](int row) {
        bool hasSelection = row >= 0;
        m_editBtn->setEnabled(hasSelection);
        m_deleteBtn->setEnabled(hasSelection);
    });

    connect(m_listWidget, &QListWidget::itemDoubleClicked,
            this, &CredentialManagerDialog::editCredential);

    refreshList();
}

void CredentialManagerDialog::refreshList()
{
    m_listWidget->clear();
    auto *userDb = Application::instance()->userDatabase();
    if (!userDb)
        return;

    auto creds = userDb->allCredentials();
    for (const auto &c : creds) {
        QString label = c.name;
        if (c.type == CredentialType::PrivateKey)
            label += QStringLiteral(" (key)");
        auto *item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, c.id);
        m_listWidget->addItem(item);
    }
}

void CredentialManagerDialog::addCredential()
{
    auto *vault = Application::instance()->vault();
    if (!vault || !vault->isUnlocked()) {
        QMessageBox::warning(this, QStringLiteral("Vault Locked"),
                             QStringLiteral("The credential vault is locked."));
        return;
    }

    CredentialDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Add Credential"));
    if (dlg.exec() != QDialog::Accepted)
        return;

    Credential cred = dlg.credential();
    if (cred.name.isEmpty())
        return;

    // Encrypt before storing
    cred = vault->encryptCredential(cred);

    auto *userDb = Application::instance()->userDatabase();
    cred.id = userDb->insertCredential(cred);

    refreshList();
}

void CredentialManagerDialog::editCredential()
{
    auto *item = m_listWidget->currentItem();
    if (!item)
        return;

    auto *vault = Application::instance()->vault();
    if (!vault || !vault->isUnlocked())
        return;

    auto *userDb = Application::instance()->userDatabase();
    qint64 credId = item->data(Qt::UserRole).toLongLong();
    Credential cred = userDb->credentialById(credId);
    cred = vault->decryptCredential(cred);

    CredentialDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Edit Credential"));
    dlg.setCredential(cred);
    if (dlg.exec() != QDialog::Accepted)
        return;

    Credential updated = dlg.credential();
    updated.id = credId;
    updated = vault->encryptCredential(updated);
    userDb->updateCredential(updated);

    refreshList();
}

void CredentialManagerDialog::deleteCredential()
{
    auto *item = m_listWidget->currentItem();
    if (!item)
        return;

    qint64 credId = item->data(Qt::UserRole).toLongLong();
    QString name = item->text();

    auto result = QMessageBox::question(this, QStringLiteral("Delete Credential"),
        QStringLiteral("Delete credential \"%1\"?").arg(name));
    if (result != QMessageBox::Yes)
        return;

    auto *userDb = Application::instance()->userDatabase();
    userDb->deleteCredential(credId);

    refreshList();
}
