#pragma once

#include <QObject>
#include <QByteArray>
#include <atomic>
#include <libssh2.h>
#include "core/ConnectionEntry.h"
#include "SshChannel.h"

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

class SshSession : public QObject {
    Q_OBJECT
public:
    explicit SshSession(const ConnectionEntry &entry,
                        const QString &username, const QString &password,
                        const QString &privateKey,
                        int cols, int rows,
                        QObject *parent = nullptr);
    ~SshSession() override;

public slots:
    void start();
    void stop();
    void write(const QByteArray &data);
    void requestPtyResize(int cols, int rows);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void dataReceived(const QByteArray &data);

private:
    void readLoop();
    void cleanup();
    bool connectTcp();
    bool performHandshake();
    bool authenticate();

    ConnectionEntry m_entry;
    QString m_username;
    QString m_password;
    QString m_privateKey;
    int m_cols;
    int m_rows;

    socket_t m_socket = kInvalidSocket;
    LIBSSH2_SESSION *m_session = nullptr;
    SshChannel m_channel;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
