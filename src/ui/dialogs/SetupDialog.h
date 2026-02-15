#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>

class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(QWidget *parent = nullptr);

    QString sharedDatabasePath() const;
    bool isAdmin() const;

private:
    void browse();
    void validate();

    QLineEdit *m_pathEdit = nullptr;
    QCheckBox *m_adminCheck = nullptr;
};
