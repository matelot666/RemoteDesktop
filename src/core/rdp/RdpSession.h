#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QRect>
#include <QTemporaryDir>
#include <QStringList>
#include <atomic>
#include <vector>
#include <memory>

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>
#include <winpr/shell.h>

#include "core/ConnectionEntry.h"

struct RemoteFileEntry {
    QString remoteName;       // relative path from FILEDESCRIPTORW (backslashes)
    QString localPath;        // resolved path in temp dir
    uint64_t fileSize = 0;
    bool isDirectory = false;
    bool sizeKnown = false;
};

struct LocalFileEntry {
    QString localPath;        // absolute path on disk
    QString remoteName;       // relative path for FILEDESCRIPTORW (backslashes)
    uint64_t fileSize = 0;
    bool isDirectory = false;
};

struct RdpOptions {
    bool gfxPipeline = false;
    bool h264 = false;
    bool remoteFx = true;
    bool fontSmoothing = true;
};

class RdpSession : public QObject {
    Q_OBJECT
public:
    static constexpr uint32_t FILE_FORMAT_ID = 0xC0A0;

    explicit RdpSession(const ConnectionEntry &entry,
                        const QString &username, const QString &password,
                        QObject *parent = nullptr);
    ~RdpSession() override;

    // Called from FreeRDP callbacks (worker thread only)
    void onPostConnect(int width, int height, uint8_t *buffer, uint32_t stride);
    void onEndPaint(const QRect &dirtyRect);

    // Thread-safe: returns deep copy of our private framebuffer
    QImage framebuffer() const;
    // Thread-safe: incrementally copy only the dirty region into dest
    void copyRegionTo(QImage &dest, const QRect &region) const;

    // Thread-safe: atomically grab accumulated dirty region, copy it into dest,
    // and clear the internal dirty flag.  Returns the dirty QRect (empty if clean).
    QRect takeDirtyRegion(QImage &dest);
    int desktopWidth() const { return m_width; }
    int desktopHeight() const { return m_height; }

    QString username() const { return m_username; }
    QString password() const { return m_password; }

    void setDesktopSize(int width, int height);
    void setRdpOptions(const RdpOptions &opts);

    // Clipboard bridge
    void setCliprdrContext(CliprdrClientContext *ctx);
    void onClipboardDataReceived(const QString &text);
    void onServerRequestsClipboard(uint32_t formatId);

    // File clipboard (called from C callbacks on worker thread)
    void onServerSupportsFileClip(bool supported);
    void onRemoteFormatList(CliprdrClientContext *cliprdr, uint32_t fileFormatId, uint32_t textFormatId);
    void onRemoteFileListReceived(FILEDESCRIPTORW *descriptors, uint32_t count);
    void onFileContentsResponse(uint32_t streamId, const BYTE *data, uint32_t size, bool success);
    void onServerFileContentsRequest(CliprdrClientContext *cliprdr, uint32_t streamId,
                                      uint32_t listIndex, uint32_t dwFlags, uint64_t offset, uint32_t cbRequested);
    void handleFormatDataRequest(CliprdrClientContext *cliprdr, uint32_t requestedFormatId);
    bool isFileFormatResponse() const;

public slots:
    void start();
    void stop();
    void sendMouseEvent(uint16_t flags, int x, int y);
    void sendKeyboardEvent(bool down, uint16_t rdpScancode, bool extended);
    void sendLocalClipboardToServer();
    void announceLocalFiles(const QStringList &filePaths);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void framebufferUpdated(const QRect &dirtyRect);
    void remoteClipboardChanged(const QString &text);
    void remoteFilesReceived(const QStringList &localPaths);

private:
    void eventLoop();

    ConnectionEntry m_entry;
    QString m_username;
    QString m_password;

    freerdp *m_instance = nullptr;
    mutable QMutex m_fbMutex;
    QImage m_framebuffer;       // Our own buffer (deep copy, not wrapping FreeRDP)
    uint8_t *m_rdpBuffer = nullptr;  // FreeRDP's GDI buffer (do NOT access from UI thread)
    uint32_t m_stride = 0;
    int m_width = 0;
    int m_height = 0;
    QRect m_accumulatedDirty;   // Union of dirty rects since last takeDirtyRegion() (guarded by m_fbMutex)
    int m_requestedWidth = 1920;
    int m_requestedHeight = 1080;
    RdpOptions m_rdpOptions;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};  // Survives start() overwriting m_running
    CliprdrClientContext *m_cliprdr = nullptr;
    bool m_serverUseLongFormatNames = false;

    // File clipboard state
    bool m_serverSupportsFileClip = false;
    uint32_t m_fileFormatId = 0;
    uint32_t m_lastRequestedFormatId = 0;

    // Remote → Local
    std::unique_ptr<QTemporaryDir> m_recvTempDir;
    std::vector<RemoteFileEntry> m_remoteFiles;
    int m_recvFileIndex = 0;
    bool m_recvWaitingForSize = false;
    QByteArray m_recvFileBuffer;
    uint64_t m_recvFileOffset = 0;

    // Local → Remote
    std::vector<LocalFileEntry> m_localFiles;
    uint32_t m_nextStreamId = 1;

    void requestNextFileContents();
    void requestFileDataChunk();
    void writeReceivedFile();
    void finishFileReceive();
    void buildLocalFileList(const QList<QUrl> &urls);
    void addDirectoryEntries(const QString &dirPath, const QString &prefix);
    void refreshLocalFilesFromClipboard();
};
