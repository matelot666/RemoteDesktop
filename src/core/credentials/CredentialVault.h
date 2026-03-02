#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include "core/Credential.h"

class UserDatabase;

class CredentialVault : public QObject {
    Q_OBJECT
public:
    explicit CredentialVault(UserDatabase *db, QObject *parent = nullptr);

    bool isUnlocked() const { return m_unlocked; }

    // First-time setup: creates salt and derives key
    bool setupMasterPassword(const QString &password);

    // Subsequent: derives key and verifies against stored check value
    bool unlock(const QString &password);
    void lock();

    // Encrypt/decrypt individual fields
    QByteArray encrypt(const QString &plaintext) const;
    QString decrypt(const QByteArray &ciphertext) const;

    // High-level: encrypt credential fields before DB storage
    Credential encryptCredential(const Credential &cred) const;
    Credential decryptCredential(const Credential &cred) const;

    // Check if vault has been set up (salt exists)
    bool hasBeenSetup() const;

    // Async unlock support — separates CPU-intensive PBKDF2 from DB I/O
    // so callers can run pbkdf2Derive() on a background thread while
    // keeping all DB access on the main thread.
    QByteArray readSalt() const;
    static QByteArray pbkdf2Derive(const QString &password, const QByteArray &salt);
    bool completeUnlock(const QByteArray &derivedKey);
    bool completeSetup(const QByteArray &derivedKey);

signals:
    void lockedChanged(bool locked);

private:
    bool deriveKey(const QString &password, const QByteArray &salt);

    UserDatabase *m_db;
    QByteArray m_derivedKey; // 32 bytes AES-256 key
    bool m_unlocked = false;

    static constexpr int SALT_LENGTH = 16;
    static constexpr int KEY_LENGTH = 32;
    static constexpr int IV_LENGTH = 12;
    static constexpr int TAG_LENGTH = 16;
    static constexpr int PBKDF2_ITERATIONS = 600000;
};
