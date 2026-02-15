#pragma once

#include <QtTypes>

enum class Protocol : int {
    RDP = 0,
    SSH = 1
};

enum class ConnectionState : int {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Error = 3
};

enum class CredentialType : int {
    UsernamePassword = 0,
    PrivateKey = 1
};

enum class TreeNodeType : int {
    Folder = 0,
    Connection = 1
};
