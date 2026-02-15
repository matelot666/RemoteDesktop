#include "FolderDialog.h"
#include "app/Application.h"
#include "core/userdb/UserDatabase.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

FolderDialog::FolderDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Folder"));
    setMinimumWidth(350);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_nameEdit = new QLineEdit;
    form->addRow(QStringLiteral("Name:"), m_nameEdit);

    // Populate credential combos from user database
    auto *userDb = Application::instance()->userDatabase();
    QVector<Credential> creds;
    if (userDb)
        creds = userDb->allCredentials();

    m_rdpCredentialCombo = new QComboBox;
    m_rdpCredentialCombo->addItem(QStringLiteral("(None)"), -1);
    for (const auto &c : creds)
        m_rdpCredentialCombo->addItem(c.name, c.id);
    form->addRow(QStringLiteral("Default RDP Credential:"), m_rdpCredentialCombo);

    m_sshCredentialCombo = new QComboBox;
    m_sshCredentialCombo->addItem(QStringLiteral("(None)"), -1);
    for (const auto &c : creds)
        m_sshCredentialCombo->addItem(c.name, c.id);
    form->addRow(QStringLiteral("Default SSH Credential:"), m_sshCredentialCombo);

    layout->addLayout(form);

    m_forceInheritanceCheck = new QCheckBox(QStringLiteral("Force inherit to all sub-folders and connections"));
    layout->addWidget(m_forceInheritanceCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void FolderDialog::setFolder(const ConnectionFolder &folder)
{
    m_folder = folder;
    m_nameEdit->setText(folder.name);

    int rdpIdx = m_rdpCredentialCombo->findData(folder.defaultRdpCredentialId);
    if (rdpIdx >= 0)
        m_rdpCredentialCombo->setCurrentIndex(rdpIdx);

    int sshIdx = m_sshCredentialCombo->findData(folder.defaultSshCredentialId);
    if (sshIdx >= 0)
        m_sshCredentialCombo->setCurrentIndex(sshIdx);
}

ConnectionFolder FolderDialog::folder() const
{
    ConnectionFolder f = m_folder;
    f.name = m_nameEdit->text().trimmed();
    f.defaultRdpCredentialId = m_rdpCredentialCombo->currentData().toLongLong();
    f.defaultSshCredentialId = m_sshCredentialCombo->currentData().toLongLong();
    return f;
}

bool FolderDialog::forceInheritance() const
{
    return m_forceInheritanceCheck->isChecked();
}

void FolderDialog::setReadOnlyName(bool readOnly)
{
    m_nameEdit->setReadOnly(readOnly);
    // Credential combos and force-inherit stay enabled for non-admins
}
