#pragma once

#include <QString>
#include <QtTypes>

struct ConnectionFolder {
    qint64 id = -1;
    QString name;
    qint64 parentId = -1; // -1 = root level
    int sortOrder = 0;
    qint64 defaultRdpCredentialId = -1;  // -1 = none
    qint64 defaultSshCredentialId = -1;  // -1 = none
};
