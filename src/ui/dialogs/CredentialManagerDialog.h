#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include "core/Credential.h"

class CredentialManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit CredentialManagerDialog(QWidget *parent = nullptr);

private:
    void refreshList();
    void addCredential();
    void editCredential();
    void deleteCredential();

    QListWidget *m_listWidget = nullptr;
    QPushButton *m_editBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
};
