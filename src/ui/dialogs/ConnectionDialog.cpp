#include "ConnectionDialog.h"
#include "app/Application.h"
#include "core/userdb/UserDatabase.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Connection"));
    setMinimumWidth(400);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_nameEdit = new QLineEdit;
    form->addRow(QStringLiteral("Name:"), m_nameEdit);

    m_protocolCombo = new QComboBox;
    m_protocolCombo->addItem(QStringLiteral("RDP"), static_cast<int>(Protocol::RDP));
    m_protocolCombo->addItem(QStringLiteral("SSH"), static_cast<int>(Protocol::SSH));
    connect(m_protocolCombo, &QComboBox::currentIndexChanged,
            this, &ConnectionDialog::onProtocolChanged);
    form->addRow(QStringLiteral("Protocol:"), m_protocolCombo);

    m_hostnameEdit = new QLineEdit;
    form->addRow(QStringLiteral("Hostname:"), m_hostnameEdit);

    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(3389);
    form->addRow(QStringLiteral("Port:"), m_portSpin);

    m_colorDepthCombo = new QComboBox;
    m_colorDepthCombo->addItem(QStringLiteral("High Color (15 bit)"), 15);
    m_colorDepthCombo->addItem(QStringLiteral("High Color (16 bit)"), 16);
    m_colorDepthCombo->addItem(QStringLiteral("True Color (24 bit)"), 24);
    m_colorDepthCombo->addItem(QStringLiteral("Highest Quality (32 bit)"), 32);
    m_colorDepthCombo->setCurrentIndex(3); // 32-bit default
    m_colorDepthLabel = new QLabel(QStringLiteral("Color Depth:"));
    form->addRow(m_colorDepthLabel, m_colorDepthCombo);

    m_credentialCombo = new QComboBox;
    m_credentialCombo->addItem(QStringLiteral("(None)"), -1);
    // Populate credentials from user database
    auto *userDb = Application::instance()->userDatabase();
    if (userDb) {
        auto creds = userDb->allCredentials();
        for (const auto &c : creds) {
            m_credentialCombo->addItem(c.name, c.id);
        }
    }
    form->addRow(QStringLiteral("Credential:"), m_credentialCombo);

    layout->addLayout(form);

    // RDP Options group (only visible for RDP connections)
    m_rdpOptionsGroup = new QGroupBox(QStringLiteral("RDP Options"));
    auto *rdpLayout = new QVBoxLayout(m_rdpOptionsGroup);

    m_clipboardCheck = new QCheckBox(QStringLiteral("Enable clipboard"));
    m_clipboardCheck->setChecked(true);
    rdpLayout->addWidget(m_clipboardCheck);

    m_compressionCheck = new QCheckBox(QStringLiteral("Enable compression"));
    m_compressionCheck->setChecked(true);
    rdpLayout->addWidget(m_compressionCheck);

    m_winKeyPassthroughCheck = new QCheckBox(QStringLiteral("Windows key pass-through"));
    m_winKeyPassthroughCheck->setChecked(true);
    rdpLayout->addWidget(m_winKeyPassthroughCheck);

    m_winKeyCopyPasteCheck = new QCheckBox(QStringLiteral("Windows key copy/cut/paste"));
    m_winKeyCopyPasteCheck->setChecked(true);
    rdpLayout->addWidget(m_winKeyCopyPasteCheck);

    auto *securityRow = new QHBoxLayout;
    securityRow->addWidget(new QLabel(QStringLiteral("Security:")));
    m_securityCombo = new QComboBox;
    m_securityCombo->addItem(QStringLiteral("Auto (Negotiate)"), static_cast<int>(RdpSecurity::Auto));
    m_securityCombo->addItem(QStringLiteral("NLA (CredSSP)"), static_cast<int>(RdpSecurity::NLA));
    m_securityCombo->addItem(QStringLiteral("TLS"), static_cast<int>(RdpSecurity::TLS));
    m_securityCombo->addItem(QStringLiteral("RDP"), static_cast<int>(RdpSecurity::RDP));
    securityRow->addWidget(m_securityCombo);
    rdpLayout->addLayout(securityRow);

    layout->addWidget(m_rdpOptionsGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ConnectionDialog::setConnection(const ConnectionEntry &entry)
{
    m_entry = entry;
    m_nameEdit->setText(entry.name);
    m_protocolCombo->setCurrentIndex(static_cast<int>(entry.protocol));
    m_hostnameEdit->setText(entry.hostname);
    m_portSpin->setValue(entry.port);

    int credIdx = m_credentialCombo->findData(entry.credentialId);
    if (credIdx >= 0)
        m_credentialCombo->setCurrentIndex(credIdx);

    int depthIdx = m_colorDepthCombo->findData(entry.colorDepth);
    if (depthIdx >= 0)
        m_colorDepthCombo->setCurrentIndex(depthIdx);

    m_clipboardCheck->setChecked(entry.enableClipboard);
    m_compressionCheck->setChecked(entry.enableCompression);
    m_winKeyPassthroughCheck->setChecked(entry.winKeyPassthrough);
    m_winKeyCopyPasteCheck->setChecked(entry.winKeyCopyPaste);

    int secIdx = m_securityCombo->findData(static_cast<int>(entry.securityMode));
    if (secIdx >= 0)
        m_securityCombo->setCurrentIndex(secIdx);

    bool isRdp = entry.protocol == Protocol::RDP;
    m_colorDepthCombo->setVisible(isRdp);
    m_colorDepthLabel->setVisible(isRdp);
    m_rdpOptionsGroup->setVisible(isRdp);
}

ConnectionEntry ConnectionDialog::connection() const
{
    ConnectionEntry e = m_entry;
    e.name = m_nameEdit->text().trimmed();
    e.protocol = static_cast<Protocol>(m_protocolCombo->currentData().toInt());
    e.hostname = m_hostnameEdit->text().trimmed();
    e.port = m_portSpin->value();
    e.credentialId = m_credentialCombo->currentData().toLongLong();
    e.colorDepth = m_colorDepthCombo->currentData().toInt();
    e.enableClipboard = m_clipboardCheck->isChecked();
    e.enableCompression = m_compressionCheck->isChecked();
    e.winKeyPassthrough = m_winKeyPassthroughCheck->isChecked();
    e.winKeyCopyPaste = m_winKeyCopyPasteCheck->isChecked();
    e.securityMode = static_cast<RdpSecurity>(m_securityCombo->currentData().toInt());
    return e;
}

void ConnectionDialog::setReadOnlySharedFields(bool readOnly)
{
    m_nameEdit->setReadOnly(readOnly);
    m_protocolCombo->setEnabled(!readOnly);
    m_hostnameEdit->setReadOnly(readOnly);
    m_portSpin->setReadOnly(readOnly);
    m_colorDepthCombo->setEnabled(!readOnly);
    m_rdpOptionsGroup->setEnabled(!readOnly);
    // Credential combo stays enabled for non-admins
}

void ConnectionDialog::onProtocolChanged(int index)
{
    auto proto = static_cast<Protocol>(m_protocolCombo->itemData(index).toInt());
    m_portSpin->setValue(proto == Protocol::SSH ? 22 : 3389);

    bool isRdp = proto == Protocol::RDP;
    m_colorDepthCombo->setVisible(isRdp);
    m_colorDepthLabel->setVisible(isRdp);
    m_rdpOptionsGroup->setVisible(isRdp);
}
