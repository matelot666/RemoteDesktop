#include "QuickConnectDialog.h"
#include "app/Application.h"
#include "core/userdb/UserDatabase.h"
#include "core/credentials/CredentialVault.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QStackedWidget>

QuickConnectDialog::QuickConnectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Quick Connect"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    // Protocol
    m_protocolCombo = new QComboBox;
    m_protocolCombo->addItem(QStringLiteral("RDP"), static_cast<int>(Protocol::RDP));
    m_protocolCombo->addItem(QStringLiteral("SSH"), static_cast<int>(Protocol::SSH));
    connect(m_protocolCombo, &QComboBox::currentIndexChanged,
            this, &QuickConnectDialog::onProtocolChanged);
    form->addRow(QStringLiteral("Protocol:"), m_protocolCombo);

    // Hostname
    m_hostnameEdit = new QLineEdit;
    m_hostnameEdit->setPlaceholderText(QStringLiteral("server.example.com"));
    form->addRow(QStringLiteral("Hostname:"), m_hostnameEdit);

    // Port
    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(3389);
    form->addRow(QStringLiteral("Port:"), m_portSpin);

    layout->addLayout(form);

    // ── Credentials group ────────────────────────────────────────────
    auto *credGroup = new QGroupBox(QStringLiteral("Credentials"));
    auto *credLayout = new QVBoxLayout(credGroup);

    m_credentialModeCombo = new QComboBox;
    m_credentialModeCombo->addItem(QStringLiteral("Saved Credential"));
    m_credentialModeCombo->addItem(QStringLiteral("Enter Manually"));
    connect(m_credentialModeCombo, &QComboBox::currentIndexChanged,
            this, &QuickConnectDialog::onCredentialModeChanged);
    credLayout->addWidget(m_credentialModeCombo);

    // Saved credential picker
    m_savedCredentialWidget = new QWidget;
    auto *savedLayout = new QFormLayout(m_savedCredentialWidget);
    savedLayout->setContentsMargins(0, 4, 0, 0);
    m_savedCredentialCombo = new QComboBox;
    m_savedCredentialCombo->addItem(QStringLiteral("(None)"), static_cast<qint64>(-1));
    auto *userDb = Application::instance()->userDatabase();
    if (userDb) {
        auto creds = userDb->allCredentials();
        for (const auto &c : creds)
            m_savedCredentialCombo->addItem(c.name, c.id);
    }
    savedLayout->addRow(QStringLiteral("Credential:"), m_savedCredentialCombo);
    credLayout->addWidget(m_savedCredentialWidget);

    // Ad-hoc credential fields
    m_adHocCredentialWidget = new QWidget;
    auto *adHocLayout = new QFormLayout(m_adHocCredentialWidget);
    adHocLayout->setContentsMargins(0, 4, 0, 0);
    m_usernameEdit = new QLineEdit;
    adHocLayout->addRow(QStringLiteral("Username:"), m_usernameEdit);
    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    adHocLayout->addRow(QStringLiteral("Password:"), m_passwordEdit);

    m_privateKeyLabel = new QLabel(QStringLiteral("Private Key:"));
    auto *keyRow = new QHBoxLayout;
    m_privateKeyEdit = new QLineEdit;
    m_privateKeyEdit->setPlaceholderText(QStringLiteral("/path/to/id_rsa"));
    keyRow->addWidget(m_privateKeyEdit);
    m_browseKeyButton = new QPushButton(QStringLiteral("Browse..."));
    connect(m_browseKeyButton, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Private Key"),
            QString(), QStringLiteral("All Files (*)"));
        if (!path.isEmpty())
            m_privateKeyEdit->setText(path);
    });
    keyRow->addWidget(m_browseKeyButton);
    adHocLayout->addRow(m_privateKeyLabel, keyRow);

    m_adHocCredentialWidget->setVisible(false);
    credLayout->addWidget(m_adHocCredentialWidget);

    layout->addWidget(credGroup);

    // ── RDP Options group ────────────────────────────────────────────
    m_rdpOptionsGroup = new QGroupBox(QStringLiteral("RDP Options"));
    auto *rdpLayout = new QVBoxLayout(m_rdpOptionsGroup);

    auto *rdpForm = new QFormLayout;
    m_colorDepthCombo = new QComboBox;
    m_colorDepthCombo->addItem(QStringLiteral("High Color (15 bit)"), 15);
    m_colorDepthCombo->addItem(QStringLiteral("High Color (16 bit)"), 16);
    m_colorDepthCombo->addItem(QStringLiteral("True Color (24 bit)"), 24);
    m_colorDepthCombo->addItem(QStringLiteral("Highest Quality (32 bit)"), 32);
    m_colorDepthCombo->setCurrentIndex(3);
    rdpForm->addRow(QStringLiteral("Color Depth:"), m_colorDepthCombo);

    m_securityCombo = new QComboBox;
    m_securityCombo->addItem(QStringLiteral("Auto (Negotiate)"), static_cast<int>(RdpSecurity::Auto));
    m_securityCombo->addItem(QStringLiteral("NLA (CredSSP)"), static_cast<int>(RdpSecurity::NLA));
    m_securityCombo->addItem(QStringLiteral("TLS"), static_cast<int>(RdpSecurity::TLS));
    m_securityCombo->addItem(QStringLiteral("RDP"), static_cast<int>(RdpSecurity::RDP));
    rdpForm->addRow(QStringLiteral("Security:"), m_securityCombo);
    rdpLayout->addLayout(rdpForm);

    m_clipboardCheck = new QCheckBox(QStringLiteral("Enable clipboard"));
    m_clipboardCheck->setChecked(true);
    rdpLayout->addWidget(m_clipboardCheck);

    m_compressionCheck = new QCheckBox(QStringLiteral("Enable compression"));
    m_compressionCheck->setChecked(true);
    rdpLayout->addWidget(m_compressionCheck);

    layout->addWidget(m_rdpOptionsGroup);

    // ── Buttons ──────────────────────────────────────────────────────
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Connect"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Set initial visibility for SSH private key fields
    onProtocolChanged(0);
}

void QuickConnectDialog::accept()
{
    // Validate hostname
    if (m_hostnameEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Quick Connect"),
                             QStringLiteral("Please enter a hostname."));
        m_hostnameEdit->setFocus();
        return;
    }

    // Resolve credentials
    if (m_credentialModeCombo->currentIndex() == 0) {
        // Saved credential
        qint64 credId = m_savedCredentialCombo->currentData().toLongLong();
        if (credId > 0) {
            auto *vault = Application::instance()->vault();
            if (vault && vault->isUnlocked()) {
                auto *userDb = Application::instance()->userDatabase();
                auto cred = userDb->credentialById(credId);
                cred = vault->decryptCredential(cred);
                m_resolvedUsername = cred.username;
                m_resolvedPassword = cred.password;
                m_resolvedPrivateKey = cred.privateKey;
            } else {
                QMessageBox::warning(this, QStringLiteral("Quick Connect"),
                                     QStringLiteral("Credential vault is locked."));
                return;
            }
        }
    } else {
        // Ad-hoc credentials
        m_resolvedUsername = m_usernameEdit->text();
        m_resolvedPassword = m_passwordEdit->text();
        m_resolvedPrivateKey = m_privateKeyEdit->text();
    }

    QDialog::accept();
}

void QuickConnectDialog::onProtocolChanged(int index)
{
    auto proto = static_cast<Protocol>(m_protocolCombo->itemData(index).toInt());
    m_portSpin->setValue(proto == Protocol::SSH ? 22 : 3389);

    bool isRdp = proto == Protocol::RDP;
    m_rdpOptionsGroup->setVisible(isRdp);

    bool isSsh = proto == Protocol::SSH;
    bool adHoc = m_credentialModeCombo->currentIndex() == 1;
    m_privateKeyLabel->setVisible(isSsh && adHoc);
    m_privateKeyEdit->setVisible(isSsh && adHoc);
    m_browseKeyButton->setVisible(isSsh && adHoc);

    // Resize dialog to fit content
    adjustSize();
}

void QuickConnectDialog::onCredentialModeChanged(int index)
{
    bool adHoc = index == 1;
    m_savedCredentialWidget->setVisible(!adHoc);
    m_adHocCredentialWidget->setVisible(adHoc);

    bool isSsh = static_cast<Protocol>(m_protocolCombo->currentData().toInt()) == Protocol::SSH;
    m_privateKeyLabel->setVisible(isSsh && adHoc);
    m_privateKeyEdit->setVisible(isSsh && adHoc);
    m_browseKeyButton->setVisible(isSsh && adHoc);

    adjustSize();
}

ConnectionEntry QuickConnectDialog::connectionEntry() const
{
    ConnectionEntry e;
    e.id = -1;
    e.protocol = static_cast<Protocol>(m_protocolCombo->currentData().toInt());
    e.hostname = m_hostnameEdit->text().trimmed();
    e.name = e.hostname;
    e.port = m_portSpin->value();
    e.colorDepth = m_colorDepthCombo->currentData().toInt();
    e.securityMode = static_cast<RdpSecurity>(m_securityCombo->currentData().toInt());
    e.enableClipboard = m_clipboardCheck->isChecked();
    e.enableCompression = m_compressionCheck->isChecked();
    return e;
}
