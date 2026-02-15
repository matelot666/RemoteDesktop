#pragma once

#include <QString>

namespace UserSchema {

inline const QString CreateMetadata = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS metadata (
        key   TEXT PRIMARY KEY,
        value BLOB
    )
)");

inline const QString CreateCredentials = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS credentials (
        id                 INTEGER PRIMARY KEY AUTOINCREMENT,
        name               TEXT NOT NULL,
        type               INTEGER NOT NULL DEFAULT 0,
        username_encrypted BLOB,
        password_encrypted BLOB,
        private_key_encrypted BLOB
    )
)");

inline const QString CreateCredentialAssignments = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS credential_assignments (
        connection_id  INTEGER PRIMARY KEY,
        credential_id  INTEGER NOT NULL
    )
)");

inline const QString CreateFolderCredentialDefaults = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS folder_credential_defaults (
        folder_id                  INTEGER PRIMARY KEY,
        default_rdp_credential_id  INTEGER NOT NULL DEFAULT -1,
        default_ssh_credential_id  INTEGER NOT NULL DEFAULT -1
    )
)");

} // namespace UserSchema
