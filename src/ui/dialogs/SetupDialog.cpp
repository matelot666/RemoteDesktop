#include "SetupDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QLabel>

SetupDialog::SetupDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("First-Time Setup"));
    setMinimumWidth(500);

    auto *layout = new QVBoxLayout(this);

    auto *infoLabel = new QLabel(
        QStringLiteral("Configure the shared connection database that all users will access."));
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto *form = new QFormLayout;

    auto *pathLayout = new QHBoxLayout;
    m_pathEdit = new QLineEdit;
    m_pathEdit->setPlaceholderText(QStringLiteral("/path/to/shared/connections.db"));
    pathLayout->addWidget(m_pathEdit);

    auto *browseBtn = new QPushButton(QStringLiteral("Browse..."));
    connect(browseBtn, &QPushButton::clicked, this, &SetupDialog::browse);
    pathLayout->addWidget(browseBtn);

    form->addRow(QStringLiteral("Shared Database:"), pathLayout);
    layout->addLayout(form);

    m_adminCheck = new QCheckBox(QStringLiteral("I am an administrator (can modify shared tree)"));
    layout->addWidget(m_adminCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SetupDialog::validate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString SetupDialog::sharedDatabasePath() const
{
    return m_pathEdit->text().trimmed();
}

bool SetupDialog::isAdmin() const
{
    return m_adminCheck->isChecked();
}

void SetupDialog::browse()
{
    QString path;
    if (m_adminCheck->isChecked()) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Shared Database Location"),
            QStringLiteral("connections.db"),
            QStringLiteral("SQLite Database (*.db);;All Files (*)"));
    } else {
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Shared Database"),
            QString(),
            QStringLiteral("SQLite Database (*.db);;All Files (*)"));
    }
    if (!path.isEmpty())
        m_pathEdit->setText(path);
}

void SetupDialog::validate()
{
    QString path = sharedDatabasePath();
    if (path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Setup"),
                             QStringLiteral("Please specify a shared database path."));
        return;
    }

    if (!isAdmin() && !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Setup"),
                             QStringLiteral("The shared database file does not exist.\n"
                                            "Non-admin users must point to an existing database."));
        return;
    }

    accept();
}
