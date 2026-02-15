#pragma once

#include <QString>

namespace Schema {

inline const QString CreateMetadata = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS metadata (
        key   TEXT PRIMARY KEY,
        value BLOB
    )
)");

inline const QString CreateFolders = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS folders (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        name       TEXT    NOT NULL,
        parent_id  INTEGER DEFAULT -1,
        sort_order INTEGER DEFAULT 0
    )
)");

inline const QString CreateConnections = QStringLiteral(R"(
    CREATE TABLE IF NOT EXISTS connections (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        name          TEXT    NOT NULL,
        protocol      INTEGER NOT NULL DEFAULT 0,
        hostname      TEXT    NOT NULL,
        port          INTEGER NOT NULL DEFAULT 3389,
        folder_id     INTEGER DEFAULT -1,
        sort_order    INTEGER DEFAULT 0,
        color_depth   INTEGER NOT NULL DEFAULT 32,
        enable_clipboard    INTEGER NOT NULL DEFAULT 1,
        enable_compression  INTEGER NOT NULL DEFAULT 1,
        win_key_passthrough INTEGER NOT NULL DEFAULT 1,
        win_key_copy_paste  INTEGER NOT NULL DEFAULT 1
    )
)");

} // namespace Schema
