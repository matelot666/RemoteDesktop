#include "SshSession.h"

#include <QDebug>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QCoreApplication>

#ifdef _WIN32
#include <ws2tcpip.h>   // getaddrinfo, freeaddrinfo, sockaddr, etc.
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

SshSession::SshSession(const ConnectionEntry &entry,
                       const QString &username, const QString &password,
                       const QString &privateKey,
                       int cols, int rows,
                       QObject *parent)
    : QObject(parent)
    , m_entry(entry)
    , m_username(username)
    , m_password(password)
    , m_privateKey(privateKey)
    , m_cols(cols)
    , m_rows(rows)
{
}

SshSession::~SshSession()
{
    // Safety net: if cleanup didn't happen in start(), do it here
    cleanup();
}

void SshSession::start()
{
    if (m_stopRequested)
        return;

    if (!connectTcp()) {
        emit errorOccurred(QStringLiteral("TCP connection failed to %1:%2")
                               .arg(m_entry.hostname).arg(m_entry.port));
        cleanup();
        return;
    }

    m_session = libssh2_session_init();
    if (!m_session) {
        emit errorOccurred(QStringLiteral("Failed to create SSH session"));
        cleanup();
        return;
    }

    libssh2_session_set_blocking(m_session, 1);

    if (!performHandshake()) {
        emit errorOccurred(QStringLiteral("SSH handshake failed"));
        cleanup();
        return;
    }

    if (!authenticate()) {
        emit errorOccurred(QStringLiteral("SSH authentication failed"));
        cleanup();
        return;
    }

    if (!m_channel.open(m_session, m_cols, m_rows)) {
        emit errorOccurred(QStringLiteral("Failed to open SSH channel"));
        cleanup();
        return;
    }

    // Switch to non-blocking for the read loop
    libssh2_session_set_blocking(m_session, 0);

    emit connected();
    m_running = true;
    readLoop();

    // Cleanup on the worker thread after readLoop() exits.
    m_running = false;
    cleanup();
}

void SshSession::stop()
{
    // Safe to call from any thread (both are std::atomic<bool>).
    m_stopRequested = true;
    m_running = false;
}

void SshSession::cleanup()
{
    m_channel.close();

    if (m_session) {
        libssh2_session_set_blocking(m_session, 1);
        libssh2_session_disconnect(m_session, "Normal shutdown");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }

    if (m_socket != kInvalidSocket) {
#ifdef _WIN32
        closesocket(m_socket);
#else
        ::close(m_socket);
#endif
        m_socket = kInvalidSocket;
    }
}

bool SshSession::connectTcp()
{
    QByteArray host = m_entry.hostname.toUtf8();

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    QByteArray portStr = QByteArray::number(m_entry.port);
    struct addrinfo *result = nullptr;

    if (getaddrinfo(host.constData(), portStr.constData(), &hints, &result) != 0) {
        return false;
    }

    m_socket = kInvalidSocket;
    for (auto *rp = result; rp; rp = rp->ai_next) {
        m_socket = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (m_socket == kInvalidSocket)
            continue;
        if (::connect(m_socket, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0)
            break;
#ifdef _WIN32
        closesocket(m_socket);
#else
        ::close(m_socket);
#endif
        m_socket = kInvalidSocket;
    }

    freeaddrinfo(result);
    return m_socket != kInvalidSocket;
}

bool SshSession::performHandshake()
{
    return libssh2_session_handshake(m_session, m_socket) == 0;
}

static QString expandTilde(const QString &path)
{
    if (path.startsWith(QLatin1Char('~')))
        return QDir::homePath() + path.mid(1);
    return path;
}

bool SshSession::authenticate()
{
    QByteArray user = m_username.toUtf8();

    if (user.isEmpty()) {
        qWarning() << "SSH: no username provided";
        return false;
    }

    // 1. Try SSH agent (most common on macOS — keys loaded via ssh-add)
    qDebug() << "SSH: trying agent authentication for" << m_username;
    {
        LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
        if (agent) {
            if (libssh2_agent_connect(agent) == 0) {
                if (libssh2_agent_list_identities(agent) == 0) {
                    struct libssh2_agent_publickey *identity = nullptr;
                    struct libssh2_agent_publickey *prev = nullptr;
                    while (libssh2_agent_get_identity(agent, &identity, prev) == 0) {
                        if (libssh2_agent_userauth(agent, user.constData(), identity) == 0) {
                            qDebug() << "SSH: agent auth succeeded with key:" << identity->comment;
                            libssh2_agent_disconnect(agent);
                            libssh2_agent_free(agent);
                            return true;
                        }
                        prev = identity;
                    }
                }
                libssh2_agent_disconnect(agent);
            }
            libssh2_agent_free(agent);
        }
        qDebug() << "SSH: agent auth failed or no agent available";
    }

    // 2. Try private key file if provided
    if (!m_privateKey.isEmpty()) {
        QString keyPath = expandTilde(m_privateKey);

        if (QFile::exists(keyPath)) {
            // It's a file path
            QByteArray keyPathUtf8 = keyPath.toUtf8();
            QByteArray passphrase = m_password.toUtf8();

            qDebug() << "SSH: trying key file:" << keyPath;

            int rc = libssh2_userauth_publickey_fromfile(
                m_session, user.constData(),
                nullptr,  // public key path (auto-derived by appending .pub)
                keyPathUtf8.constData(),
                passphrase.isEmpty() ? nullptr : passphrase.constData());

            if (rc == 0) {
                qDebug() << "SSH: key file auth succeeded";
                return true;
            }
            qDebug() << "SSH: key file auth failed, rc=" << rc;

            // Get detailed error
            char *errmsg = nullptr;
            libssh2_session_last_error(m_session, &errmsg, nullptr, 0);
            if (errmsg)
                qDebug() << "SSH: error:" << errmsg;
        } else {
            // Try as in-memory key content
            QByteArray keyData = m_privateKey.toUtf8();
            QByteArray passphrase = m_password.toUtf8();

            qDebug() << "SSH: trying in-memory key";

            int rc = libssh2_userauth_publickey_frommemory(
                m_session, user.constData(), user.size(),
                nullptr, 0,
                keyData.constData(), keyData.size(),
                passphrase.isEmpty() ? nullptr : passphrase.constData());

            if (rc == 0) {
                qDebug() << "SSH: in-memory key auth succeeded";
                return true;
            }
            qDebug() << "SSH: in-memory key auth failed, rc=" << rc;
        }
    }

    // 3. Try default key files (~/.ssh/id_rsa, id_ed25519, etc.)
    if (m_privateKey.isEmpty()) {
        static const char *defaultKeys[] = {
            "/.ssh/id_ed25519",
            "/.ssh/id_rsa",
            "/.ssh/id_ecdsa",
            nullptr
        };

        QString home = QDir::homePath();
        for (int i = 0; defaultKeys[i]; ++i) {
            QString keyPath = home + QString::fromLatin1(defaultKeys[i]);
            if (!QFile::exists(keyPath))
                continue;

            QByteArray keyPathUtf8 = keyPath.toUtf8();
            qDebug() << "SSH: trying default key:" << keyPath;

            int rc = libssh2_userauth_publickey_fromfile(
                m_session, user.constData(),
                nullptr, keyPathUtf8.constData(), nullptr);

            if (rc == 0) {
                qDebug() << "SSH: default key auth succeeded:" << keyPath;
                return true;
            }
        }
    }

    // 4. Password authentication
    if (!m_password.isEmpty()) {
        QByteArray pass = m_password.toUtf8();
        qDebug() << "SSH: trying password auth";
        if (libssh2_userauth_password(m_session, user.constData(), pass.constData()) == 0) {
            qDebug() << "SSH: password auth succeeded";
            return true;
        }
    }

    // 5. Keyboard-interactive (uses password)
    if (!m_password.isEmpty()) {
        struct KbdCtx {
            const char *pass;
            size_t len;
        };
        QByteArray pass = m_password.toUtf8();
        KbdCtx ctx{pass.constData(), static_cast<size_t>(pass.size())};
        *libssh2_session_abstract(m_session) = &ctx;

        qDebug() << "SSH: trying keyboard-interactive";

        auto callback = [](const char *, int, const char *, int,
                           int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
                           LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract) {
            auto *c = static_cast<KbdCtx *>(*abstract);
            for (int i = 0; i < num_prompts; i++) {
                responses[i].text = strdup(c->pass);
                responses[i].length = static_cast<unsigned int>(c->len);
            }
        };

        if (libssh2_userauth_keyboard_interactive(m_session, user.constData(), callback) == 0) {
            qDebug() << "SSH: keyboard-interactive auth succeeded";
            return true;
        }
    }

    qWarning() << "SSH: all authentication methods failed";
    return false;
}

void SshSession::readLoop()
{
    char buffer[16384];

    while (m_running) {
        // Process queued signals (keyboard input, PTY resize from UI thread)
        QCoreApplication::processEvents();

#ifdef _WIN32
        WSAPOLLFD pfd;
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = WSAPoll(&pfd, 1, 16);
#else
        struct pollfd pfd;
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, 16);
#endif
        if (ret < 0) {
            emit errorOccurred(QStringLiteral("Socket poll error"));
            break;
        }

        if (!m_running)
            break;

        int bytesRead = m_channel.read(buffer, sizeof(buffer));

        if (bytesRead > 0) {
            emit dataReceived(QByteArray(buffer, bytesRead));
        } else if (bytesRead == LIBSSH2_ERROR_EAGAIN) {
            continue;
        } else if (bytesRead < 0) {
            emit errorOccurred(QStringLiteral("SSH read error"));
            break;
        } else if (bytesRead == 0) {
            if (m_channel.isOpen() && libssh2_channel_eof(m_channel.rawChannel())) {
                break;
            }
            QThread::msleep(10);
        }
    }

    m_running = false;
    emit disconnected();
}

void SshSession::write(const QByteArray &data)
{
    if (!m_running || !m_channel.isOpen())
        return;

    libssh2_session_set_blocking(m_session, 1);
    m_channel.write(data.constData(), data.size());
    libssh2_session_set_blocking(m_session, 0);
}

void SshSession::requestPtyResize(int cols, int rows)
{
    if (!m_running || !m_channel.isOpen())
        return;

    libssh2_session_set_blocking(m_session, 1);
    m_channel.requestPtyResize(cols, rows);
    libssh2_session_set_blocking(m_session, 0);
}
