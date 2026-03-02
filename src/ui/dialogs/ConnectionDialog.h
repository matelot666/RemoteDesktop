#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include "core/ConnectionEntry.h"

class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    void setConnection(const ConnectionEntry &entry);
    ConnectionEntry connection() const;

    void setReadOnlySharedFields(bool readOnly);

private:
    void onProtocolChanged(int index);

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostnameEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QComboBox *m_credentialCombo = nullptr;
    QComboBox *m_colorDepthCombo = nullptr;
    QLabel *m_colorDepthLabel = nullptr;
    QGroupBox *m_rdpOptionsGroup = nullptr;
    QCheckBox *m_clipboardCheck = nullptr;
    QCheckBox *m_compressionCheck = nullptr;
    QCheckBox *m_winKeyPassthroughCheck = nullptr;
    QCheckBox *m_winKeyCopyPasteCheck = nullptr;
    QComboBox *m_securityCombo = nullptr;
    ConnectionEntry m_entry;
};
