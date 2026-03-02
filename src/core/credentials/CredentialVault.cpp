#include "CredentialVault.h"
#include "core/userdb/UserDatabase.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <QDebug>

static const QString SALT_KEY = QStringLiteral("vault_salt");
static const QString CHECK_KEY = QStringLiteral("vault_check");
static const QString CHECK_PLAINTEXT = QStringLiteral("RemoteDesktopVault");

CredentialVault::CredentialVault(UserDatabase *db, QObject *parent)
    : QObject(parent), m_db(db)
{
}

bool CredentialVault::hasBeenSetup() const
{
    return !m_db->metadataValue(SALT_KEY).isEmpty();
}

bool CredentialVault::setupMasterPassword(const QString &password)
{
    // Generate random salt
    QByteArray salt(SALT_LENGTH, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), SALT_LENGTH) != 1) {
        qWarning() << "Failed to generate random salt";
        return false;
    }

    m_db->setMetadataValue(SALT_KEY, salt);

    if (!deriveKey(password, salt)) {
        return false;
    }

    // Encrypt a known string as verification value
    QByteArray checkValue = encrypt(CHECK_PLAINTEXT);
    if (checkValue.isEmpty()) {
        return false;
    }

    m_db->setMetadataValue(CHECK_KEY, checkValue);
    m_unlocked = true;
    emit lockedChanged(false);
    return true;
}

bool CredentialVault::unlock(const QString &password)
{
    QByteArray salt = m_db->metadataValue(SALT_KEY);
    if (salt.isEmpty()) {
        return false;
    }

    if (!deriveKey(password, salt)) {
        return false;
    }

    // Verify by decrypting the check value
    QByteArray checkCiphertext = m_db->metadataValue(CHECK_KEY);
    if (checkCiphertext.isEmpty()) {
        return false;
    }

    QString decrypted = decrypt(checkCiphertext);
    if (decrypted != CHECK_PLAINTEXT) {
        m_derivedKey.clear();
        return false;
    }

    m_unlocked = true;
    emit lockedChanged(false);
    return true;
}

void CredentialVault::lock()
{
    m_derivedKey.fill(0);
    m_derivedKey.clear();
    m_unlocked = false;
    emit lockedChanged(true);
}

QByteArray CredentialVault::readSalt() const
{
    return m_db->metadataValue(SALT_KEY);
}

QByteArray CredentialVault::pbkdf2Derive(const QString &password, const QByteArray &salt)
{
    QByteArray key(KEY_LENGTH, 0);
    QByteArray passUtf8 = password.toUtf8();

    int result = PKCS5_PBKDF2_HMAC(
        passUtf8.constData(), passUtf8.size(),
        reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
        PBKDF2_ITERATIONS,
        EVP_sha256(),
        KEY_LENGTH,
        reinterpret_cast<unsigned char *>(key.data()));

    if (result != 1) {
        qWarning() << "PBKDF2 key derivation failed";
        return {};
    }
    return key;
}

bool CredentialVault::completeUnlock(const QByteArray &derivedKey)
{
    if (derivedKey.isEmpty())
        return false;

    m_derivedKey = derivedKey;

    QByteArray checkCiphertext = m_db->metadataValue(CHECK_KEY);
    if (checkCiphertext.isEmpty()) {
        m_derivedKey.clear();
        return false;
    }

    QString decrypted = decrypt(checkCiphertext);
    if (decrypted != CHECK_PLAINTEXT) {
        m_derivedKey.clear();
        return false;
    }

    m_unlocked = true;
    emit lockedChanged(false);
    return true;
}

bool CredentialVault::completeSetup(const QByteArray &derivedKey)
{
    if (derivedKey.isEmpty())
        return false;

    m_derivedKey = derivedKey;

    QByteArray checkValue = encrypt(CHECK_PLAINTEXT);
    if (checkValue.isEmpty()) {
        m_derivedKey.clear();
        return false;
    }

    m_db->setMetadataValue(CHECK_KEY, checkValue);
    m_unlocked = true;
    emit lockedChanged(false);
    return true;
}

bool CredentialVault::deriveKey(const QString &password, const QByteArray &salt)
{
    m_derivedKey.resize(KEY_LENGTH);
    QByteArray passUtf8 = password.toUtf8();

    int result = PKCS5_PBKDF2_HMAC(
        passUtf8.constData(), passUtf8.size(),
        reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
        PBKDF2_ITERATIONS,
        EVP_sha256(),
        KEY_LENGTH,
        reinterpret_cast<unsigned char *>(m_derivedKey.data()));

    if (result != 1) {
        qWarning() << "PBKDF2 key derivation failed";
        m_derivedKey.clear();
        return false;
    }
    return true;
}

QByteArray CredentialVault::encrypt(const QString &plaintext) const
{
    if (m_derivedKey.isEmpty())
        return {};

    QByteArray ptBytes = plaintext.toUtf8();

    // Generate random IV
    QByteArray iv(IV_LENGTH, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(iv.data()), IV_LENGTH) != 1)
        return {};

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    QByteArray result;

    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            break;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1)
            break;

        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                reinterpret_cast<const unsigned char *>(m_derivedKey.constData()),
                reinterpret_cast<const unsigned char *>(iv.constData())) != 1)
            break;

        QByteArray ciphertext(ptBytes.size() + 16, 0);
        int outLen = 0;

        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()),
                              &outLen,
                              reinterpret_cast<const unsigned char *>(ptBytes.constData()),
                              ptBytes.size()) != 1)
            break;

        int totalLen = outLen;

        if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()) + outLen,
                                &outLen) != 1)
            break;

        totalLen += outLen;
        ciphertext.resize(totalLen);

        QByteArray tag(TAG_LENGTH, 0);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LENGTH, tag.data()) != 1)
            break;

        // Format: IV || ciphertext || tag
        result = iv + ciphertext + tag;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return result;
}

QString CredentialVault::decrypt(const QByteArray &ciphertext) const
{
    if (m_derivedKey.isEmpty() || ciphertext.size() < IV_LENGTH + TAG_LENGTH)
        return {};

    QByteArray iv = ciphertext.left(IV_LENGTH);
    QByteArray tag = ciphertext.right(TAG_LENGTH);
    QByteArray ct = ciphertext.mid(IV_LENGTH, ciphertext.size() - IV_LENGTH - TAG_LENGTH);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    QString result;

    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            break;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LENGTH, nullptr) != 1)
            break;

        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                reinterpret_cast<const unsigned char *>(m_derivedKey.constData()),
                reinterpret_cast<const unsigned char *>(iv.constData())) != 1)
            break;

        QByteArray plaintext(ct.size(), 0);
        int outLen = 0;

        if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()),
                              &outLen,
                              reinterpret_cast<const unsigned char *>(ct.constData()),
                              ct.size()) != 1)
            break;

        int totalLen = outLen;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LENGTH,
                                const_cast<char *>(tag.constData())) != 1)
            break;

        if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plaintext.data()) + outLen,
                                &outLen) != 1)
            break;

        totalLen += outLen;
        plaintext.resize(totalLen);

        result = QString::fromUtf8(plaintext);
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return result;
}

Credential CredentialVault::encryptCredential(const Credential &cred) const
{
    Credential encrypted = cred;
    if (!cred.username.isEmpty())
        encrypted.usernameEncrypted = encrypt(cred.username);
    if (!cred.password.isEmpty())
        encrypted.passwordEncrypted = encrypt(cred.password);
    if (!cred.privateKey.isEmpty())
        encrypted.privateKeyEncrypted = encrypt(cred.privateKey);
    // Clear plaintext
    encrypted.username.clear();
    encrypted.password.clear();
    encrypted.privateKey.clear();
    return encrypted;
}

Credential CredentialVault::decryptCredential(const Credential &cred) const
{
    Credential decrypted = cred;
    if (!cred.usernameEncrypted.isEmpty())
        decrypted.username = decrypt(cred.usernameEncrypted);
    if (!cred.passwordEncrypted.isEmpty())
        decrypted.password = decrypt(cred.passwordEncrypted);
    if (!cred.privateKeyEncrypted.isEmpty())
        decrypted.privateKey = decrypt(cred.privateKeyEncrypted);
    return decrypted;
}
