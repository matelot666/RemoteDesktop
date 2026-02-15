#pragma once

#include <libssh2.h>
#include <QByteArray>

class SshChannel {
public:
    SshChannel() = default;
    ~SshChannel();

    bool open(LIBSSH2_SESSION *session, int cols, int rows);
    void close();
    bool isOpen() const { return m_channel != nullptr; }
    LIBSSH2_CHANNEL *rawChannel() const { return m_channel; }

    int read(char *buffer, size_t bufferSize);
    int write(const char *data, size_t len);
    bool requestPtyResize(int cols, int rows);

private:
    LIBSSH2_CHANNEL *m_channel = nullptr;
};
