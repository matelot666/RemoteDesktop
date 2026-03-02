#include "SshChannel.h"
#include <QDebug>

SshChannel::~SshChannel()
{
    close();
}

bool SshChannel::open(LIBSSH2_SESSION *session, int cols, int rows)
{
    m_channel = libssh2_channel_open_session(session);
    if (!m_channel) {
        char *errmsg = nullptr;
        libssh2_session_last_error(session, &errmsg, nullptr, 0);
        qWarning() << "Failed to open SSH channel —" << (errmsg ? errmsg : "unknown error");
        return false;
    }

    if (libssh2_channel_request_pty_ex(m_channel, "xterm-256color", 14,
                                        nullptr, 0, cols, rows, 0, 0) != 0) {
        char *errmsg = nullptr;
        libssh2_session_last_error(session, &errmsg, nullptr, 0);
        qWarning() << "Failed to request PTY —" << (errmsg ? errmsg : "unknown error");
        close();
        return false;
    }

    if (libssh2_channel_shell(m_channel) != 0) {
        char *errmsg = nullptr;
        libssh2_session_last_error(session, &errmsg, nullptr, 0);
        qWarning() << "Failed to request shell —" << (errmsg ? errmsg : "unknown error");
        close();
        return false;
    }

    return true;
}

void SshChannel::close()
{
    if (m_channel) {
        libssh2_channel_send_eof(m_channel);
        libssh2_channel_wait_eof(m_channel);
        libssh2_channel_wait_closed(m_channel);
        libssh2_channel_free(m_channel);
        m_channel = nullptr;
    }
}

void SshChannel::forceClose()
{
    if (m_channel) {
        libssh2_channel_free(m_channel);
        m_channel = nullptr;
    }
}

int SshChannel::read(char *buffer, size_t bufferSize)
{
    if (!m_channel)
        return -1;
    return static_cast<int>(libssh2_channel_read(m_channel, buffer, bufferSize));
}

int SshChannel::write(const char *data, size_t len)
{
    if (!m_channel)
        return -1;
    return static_cast<int>(libssh2_channel_write(m_channel, data, len));
}

int SshChannel::requestPtyResize(int cols, int rows)
{
    if (!m_channel)
        return -1;
    return libssh2_channel_request_pty_size(m_channel, cols, rows);
}
