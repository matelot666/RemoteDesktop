#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include "core/Folder.h"

class FolderDialog : public QDialog {
    Q_OBJECT
public:
    explicit FolderDialog(QWidget *parent = nullptr);

    void setFolder(const ConnectionFolder &folder);
    ConnectionFolder folder() const;
    bool forceInheritance() const;

    void setReadOnlyName(bool readOnly);

private:
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_rdpCredentialCombo = nullptr;
    QComboBox *m_sshCredentialCombo = nullptr;
    QCheckBox *m_forceInheritanceCheck = nullptr;
    ConnectionFolder m_folder;
};
