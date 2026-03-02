#pragma once

#include <QString>
#include "Enums.h"

struct ConnectionEntry {
    qint64 id = -1;
    QString name;
    Protocol protocol = Protocol::RDP;
    QString hostname;
    int port = 3389;
    qint64 credentialId = -1;
    qint64 folderId = -1;
    int sortOrder = 0;
    int colorDepth = 32;  // 15, 16, 24, or 32
    bool enableClipboard = true;
    bool enableCompression = true;
    bool winKeyPassthrough = true;
    bool winKeyCopyPaste = true;
    RdpSecurity securityMode = RdpSecurity::Auto;

    int defaultPort() const {
        return protocol == Protocol::SSH ? 22 : 3389;
    }
};
