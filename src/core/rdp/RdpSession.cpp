#include "RdpSession.h"
#include "RdpClient.h"

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/event.h>

#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QMimeData>
#include <QGuiApplication>
#include <QClipboard>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>
#include <winpr/shell.h>
#include <winpr/file.h>
#include <freerdp/utils/cliprdr_utils.h>

#include <cstring>

RdpSession::RdpSession(const ConnectionEntry &entry,
                       const QString &username, const QString &password,
                       QObject *parent)
    : QObject(parent)
    , m_entry(entry)
    , m_username(username)
    , m_password(password)
{
}

RdpSession::~RdpSession()
{
    // Safety net: if cleanup didn't happen in start(), do it here
    if (m_instance) {
        freerdp_disconnect(m_instance);
        freerdp_context_free(m_instance);
        freerdp_free(m_instance);
        m_instance = nullptr;
    }
}

void RdpSession::setDesktopSize(int width, int height)
{
    m_requestedWidth = qMax(640, width);
    m_requestedHeight = qMax(480, height);
}

void RdpSession::setRdpOptions(const RdpOptions &opts)
{
    m_rdpOptions = opts;
}

void RdpSession::start()
{
    // If stop() was already called before start() runs, bail out immediately.
    if (m_stopRequested)
        return;

    m_instance = freerdp_new();
    if (!m_instance) {
        emit errorOccurred(QStringLiteral("Failed to create FreeRDP instance"));
        return;
    }

    m_instance->ContextSize = sizeof(RdpCustomContext);
    m_instance->PreConnect = rdp_pre_connect;
    m_instance->PostConnect = rdp_post_connect;
    m_instance->PostDisconnect = rdp_post_disconnect;
    m_instance->LoadChannels = rdp_load_channels;
    m_instance->Authenticate = rdp_authenticate;
    m_instance->AuthenticateEx = rdp_authenticate_ex;
    m_instance->VerifyCertificateEx = rdp_verify_certificate;
    m_instance->VerifyChangedCertificateEx = rdp_verify_changed_certificate;

    if (!freerdp_context_new(m_instance)) {
        emit errorOccurred(QStringLiteral("Failed to create FreeRDP context"));
        freerdp_free(m_instance);
        m_instance = nullptr;
        return;
    }

    // Store session pointer in custom context
    auto *ctx = reinterpret_cast<RdpCustomContext *>(m_instance->context);
    ctx->session = this;

    // Configure settings
    rdpSettings *settings = m_instance->context->settings;

    QByteArray host = m_entry.hostname.toUtf8();
    freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host.constData());
    freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, m_entry.port);

    if (!m_username.isEmpty()) {
        QString domain, user;

        // Parse DOMAIN\user or user@domain formats
        int backslash = m_username.indexOf(QLatin1Char('\\'));
        if (backslash > 0) {
            domain = m_username.left(backslash);
            user = m_username.mid(backslash + 1);
        } else {
            int at = m_username.indexOf(QLatin1Char('@'));
            if (at > 0) {
                user = m_username.left(at);
                domain = m_username.mid(at + 1);
            } else {
                user = m_username;
            }
        }

        freerdp_settings_set_string(settings, FreeRDP_Username, user.toUtf8().constData());
        if (!domain.isEmpty()) {
            freerdp_settings_set_string(settings, FreeRDP_Domain, domain.toUtf8().constData());
        }
    }
    if (!m_password.isEmpty()) {
        QByteArray pass = m_password.toUtf8();
        freerdp_settings_set_string(settings, FreeRDP_Password, pass.constData());
    }

    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, m_requestedWidth);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, m_requestedHeight);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, m_entry.colorDepth);
    freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);

    // Compression
    freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, m_entry.enableCompression ? TRUE : FALSE);

    // Clipboard redirection
    freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, m_entry.enableClipboard ? TRUE : FALSE);

    // ── Performance: caching (safe with GDI rendering path) ──
    freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheV3Enabled, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_OffscreenSupportLevel, 1);
    freerdp_settings_set_uint32(settings, FreeRDP_GlyphSupportLevel, 3); // GLYPH_SUPPORT_ENCODE

    // ── Performance: codecs ──
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, m_rdpOptions.remoteFx ? TRUE : FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);

    // ── GFX pipeline (off by default — requires RDPGFX surface rendering) ──
    if (m_rdpOptions.gfxPipeline) {
        freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxSendQoeAck, TRUE);
        if (m_rdpOptions.h264) {
            freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
        } else {
            freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
        }
    }

    // ── Performance: fast path, frame acks, network tuning ──
    freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);

    // ── Performance: visual experience ──
    freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_PerformanceFlags, 0); // All visual features on
    freerdp_settings_set_uint32(settings, FreeRDP_LargePointerFlag, 1);

    // Accept all certificates (self-signed, changed, etc.)
    freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

    if (!m_username.isEmpty()) {
        // Credentials provided: enable NLA + auto logon
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, TRUE);
    } else {
        // No credentials: disable NLA so the server shows its login screen
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    }

    // Enable standard security negotiation
    freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE);

    // Subscribe to PubSub channel events BEFORE freerdp_connect.
    // freerdp_channels_post_connect (called inside freerdp_connect) fires
    // PubSub_OnChannelConnected for each channel — this is the most reliable
    // way to capture the CliprdrClientContext interface.
    PubSub_SubscribeChannelConnected(m_instance->context->pubSub,
                                     rdp_on_channel_connected);
    PubSub_SubscribeChannelDisconnected(m_instance->context->pubSub,
                                        rdp_on_channel_disconnected);

    // Attempt connection
    if (!freerdp_connect(m_instance)) {
        UINT32 err = freerdp_get_last_error(m_instance->context);
        const char *errName = freerdp_get_last_error_name(err);
        const char *errString = freerdp_get_last_error_string(err);
        emit errorOccurred(QStringLiteral("RDP connect failed: 0x%1 (%2: %3)")
                               .arg(err, 8, 16, QChar('0'))
                               .arg(QString::fromUtf8(errName ? errName : "unknown"))
                               .arg(QString::fromUtf8(errString ? errString : "")));
        // Must disconnect before freeing context to clean up partial state
        freerdp_disconnect(m_instance);
        freerdp_context_free(m_instance);
        freerdp_free(m_instance);
        m_instance = nullptr;
        return;
    }

    // Check if stop() was called while freerdp_connect() was blocking
    if (m_stopRequested) {
        freerdp_disconnect(m_instance);
        freerdp_context_free(m_instance);
        freerdp_free(m_instance);
        m_instance = nullptr;
        return;
    }

    if (m_entry.enableClipboard && !m_cliprdr) {
        qWarning() << "Clipboard channel was not negotiated — clipboard won't work";
    }

    emit connected();
    m_running = true;
    eventLoop();

    // Cleanup on the worker thread after eventLoop() exits.
    // This is the only place FreeRDP resources should be freed during normal operation.
    m_running = false;
    if (m_instance) {
        freerdp_disconnect(m_instance);
        freerdp_context_free(m_instance);
        freerdp_free(m_instance);
        m_instance = nullptr;
    }
}

void RdpSession::stop()
{
    // Safe to call from any thread (both are std::atomic<bool>).
    m_stopRequested = true;
    m_running = false;
}

void RdpSession::eventLoop()
{
    while (m_running && m_instance) {
        // Process queued signals (mouse/keyboard input from UI thread)
        QCoreApplication::processEvents();

        HANDLE handles[64] = {};
        DWORD count = freerdp_get_event_handles(m_instance->context, handles, 64);
        if (count == 0) {
            emit errorOccurred(QStringLiteral("Failed to get event handles"));
            break;
        }

        DWORD waitResult = WaitForMultipleObjects(count, handles, FALSE, 16);

        if (!m_running)
            break;

        if (waitResult == WAIT_FAILED) {
            emit errorOccurred(QStringLiteral("WaitForMultipleObjects failed"));
            break;
        }

        if (!freerdp_check_event_handles(m_instance->context)) {
            if (freerdp_get_last_error(m_instance->context) != FREERDP_ERROR_SUCCESS) {
                emit errorOccurred(QStringLiteral("Connection lost"));
            }
            break;
        }
    }

    m_running = false;
    emit disconnected();
}

void RdpSession::onPostConnect(int width, int height, uint8_t *buffer, uint32_t stride)
{
    QMutexLocker lock(&m_fbMutex);
    m_width = width;
    m_height = height;
    m_rdpBuffer = buffer;
    m_stride = stride;

    // Create our OWN framebuffer (deep copy). The UI thread reads from this,
    // never from the FreeRDP GDI buffer directly.
    m_framebuffer = QImage(m_width, m_height, QImage::Format_ARGB32);
    m_framebuffer.fill(Qt::black);

    // Copy initial frame from FreeRDP buffer
    const int rowBytes = m_width * 4; // BGRA32 = 4 bytes per pixel
    for (int y = 0; y < m_height; ++y) {
        memcpy(m_framebuffer.scanLine(y),
               m_rdpBuffer + static_cast<ptrdiff_t>(y) * m_stride,
               rowBytes);
    }
}

void RdpSession::onEndPaint(const QRect &dirtyRect)
{
    // Called on worker thread from FreeRDP's EndPaint callback.
    // Copy only the dirty region from FreeRDP's buffer into our own buffer.
    QMutexLocker lock(&m_fbMutex);

    if (m_framebuffer.isNull() || !m_rdpBuffer)
        return;

    // Clamp dirty rect to buffer dimensions
    int x0 = qMax(0, dirtyRect.x());
    int y0 = qMax(0, dirtyRect.y());
    int x1 = qMin(m_width, dirtyRect.x() + dirtyRect.width());
    int y1 = qMin(m_height, dirtyRect.y() + dirtyRect.height());

    if (x0 >= x1 || y0 >= y1)
        return;

    const int copyBytes = (x1 - x0) * 4; // BGRA32
    for (int y = y0; y < y1; ++y) {
        const uint8_t *src = m_rdpBuffer + static_cast<ptrdiff_t>(y) * m_stride + x0 * 4;
        uint8_t *dst = m_framebuffer.scanLine(y) + x0 * 4;
        memcpy(dst, src, copyBytes);
    }

    // Accumulate dirty region — the UI thread polls via takeDirtyRegion()
    QRect clamped(x0, y0, x1 - x0, y1 - y0);
    if (m_accumulatedDirty.isEmpty())
        m_accumulatedDirty = clamped;
    else
        m_accumulatedDirty = m_accumulatedDirty.united(clamped);
}

QImage RdpSession::framebuffer() const
{
    QMutexLocker lock(&m_fbMutex);
    return m_framebuffer.copy(); // Deep copy of OUR buffer (safe from any thread)
}

void RdpSession::copyRegionTo(QImage &dest, const QRect &region) const
{
    QMutexLocker lock(&m_fbMutex);

    if (m_framebuffer.isNull())
        return;

    // (Re)initialize dest if size/format mismatch (first call or desktop resize)
    if (dest.size() != m_framebuffer.size() || dest.format() != m_framebuffer.format()) {
        dest = m_framebuffer.copy();
        return;
    }

    // Clamp region to buffer bounds
    int x0 = qMax(0, region.x());
    int y0 = qMax(0, region.y());
    int x1 = qMin(m_framebuffer.width(), region.x() + region.width());
    int y1 = qMin(m_framebuffer.height(), region.y() + region.height());

    if (x0 >= x1 || y0 >= y1)
        return;

    const int copyBytes = (x1 - x0) * 4; // ARGB32 = 4 bytes per pixel
    for (int y = y0; y < y1; ++y) {
        const uchar *src = m_framebuffer.constScanLine(y) + x0 * 4;
        uchar *dst = dest.scanLine(y) + x0 * 4;
        memcpy(dst, src, copyBytes);
    }
}

QRect RdpSession::takeDirtyRegion(QImage &dest)
{
    QMutexLocker lock(&m_fbMutex);

    if (m_framebuffer.isNull() || m_accumulatedDirty.isEmpty())
        return {};

    // Grab and clear the accumulated dirty region
    QRect dirty = m_accumulatedDirty;
    m_accumulatedDirty = QRect();

    // (Re)initialize dest if size/format mismatch (first call or desktop resize)
    if (dest.size() != m_framebuffer.size() || dest.format() != m_framebuffer.format()) {
        dest = m_framebuffer.copy();
        return QRect(0, 0, m_framebuffer.width(), m_framebuffer.height());
    }

    // Copy only the dirty region
    int x0 = qMax(0, dirty.x());
    int y0 = qMax(0, dirty.y());
    int x1 = qMin(m_framebuffer.width(), dirty.x() + dirty.width());
    int y1 = qMin(m_framebuffer.height(), dirty.y() + dirty.height());

    if (x0 >= x1 || y0 >= y1)
        return {};

    const int copyBytes = (x1 - x0) * 4;
    for (int y = y0; y < y1; ++y) {
        const uchar *src = m_framebuffer.constScanLine(y) + x0 * 4;
        uchar *dst = dest.scanLine(y) + x0 * 4;
        memcpy(dst, src, copyBytes);
    }

    return QRect(x0, y0, x1 - x0, y1 - y0);
}

void RdpSession::sendMouseEvent(uint16_t flags, int x, int y)
{
    if (!m_instance || !m_running)
        return;
    rdpInput *input = m_instance->context->input;
    freerdp_input_send_mouse_event(input, flags, x, y);
}

void RdpSession::sendKeyboardEvent(bool down, uint16_t rdpScancode, bool extended)
{
    if (!m_instance || !m_running)
        return;
    rdpInput *input = m_instance->context->input;
    uint16_t flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
    if (extended)
        flags |= KBD_FLAGS_EXTENDED;
    freerdp_input_send_keyboard_event(input, flags, rdpScancode);
}

// ── Clipboard bridge ────────────────────────────────────────────────

void RdpSession::setCliprdrContext(CliprdrClientContext *ctx)
{
    m_cliprdr = ctx;
}

void RdpSession::onClipboardDataReceived(const QString &text)
{
    // Called from cliprdr callback on the worker thread.
    // Emit signal so the UI thread can update QClipboard.
    emit remoteClipboardChanged(text);
}

void RdpSession::onServerRequestsClipboard(uint32_t formatId)
{
    Q_UNUSED(formatId)
    // Server wants our clipboard data. This is handled in the C callback
    // (rdp_cliprdr_server_format_data_request) which reads QClipboard directly.
}

void RdpSession::sendLocalClipboardToServer()
{
    if (!m_cliprdr)
        return;

    // Always announce both FileGroupDescriptorW and CF_UNICODETEXT.
    // Actual data is served lazily in handleFormatDataRequest() — if the
    // clipboard has file URLs at paste time, we serve files; otherwise text.
    CLIPRDR_FORMAT formats[2] = {};
    formats[0].formatId = FILE_FORMAT_ID;
    formats[0].formatName = const_cast<char *>("FileGroupDescriptorW");
    formats[1].formatId = 13; // CF_UNICODETEXT
    formats[1].formatName = nullptr;

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 2;
    formatList.formats = formats;

    if (m_cliprdr->ClientFormatList)
        m_cliprdr->ClientFormatList(m_cliprdr, &formatList);
}

void RdpSession::announceLocalFiles(const QStringList &filePaths)
{
    if (!m_cliprdr)
        return;

    // Feedback loop prevention: skip if all paths are inside our receive temp dir
    if (m_recvTempDir) {
        QString tempPath = m_recvTempDir->path();
        bool allInTemp = true;
        for (const QString &path : filePaths) {
            if (!path.startsWith(tempPath)) {
                allInTemp = false;
                break;
            }
        }
        if (allInTemp)
            return;
    }

    // Build file list from paths
    QList<QUrl> urls;
    for (const QString &path : filePaths)
        urls.append(QUrl::fromLocalFile(path));
    buildLocalFileList(urls);

    if (m_localFiles.empty())
        return;

    // Announce FileGroupDescriptorW + CF_UNICODETEXT
    CLIPRDR_FORMAT formats[2] = {};
    formats[0].formatId = FILE_FORMAT_ID;
    formats[0].formatName = const_cast<char *>("FileGroupDescriptorW");
    formats[1].formatId = 13; // CF_UNICODETEXT
    formats[1].formatName = nullptr;

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 2;
    formatList.formats = formats;

    if (m_cliprdr->ClientFormatList)
        m_cliprdr->ClientFormatList(m_cliprdr, &formatList);
}

// ── File clipboard: server capability ────────────────────────────────

void RdpSession::onServerSupportsFileClip(bool supported)
{
    m_serverSupportsFileClip = supported;
}

bool RdpSession::isFileFormatResponse() const
{
    return m_lastRequestedFormatId != 0 && m_lastRequestedFormatId == m_fileFormatId;
}

// ── File clipboard: remote format list ───────────────────────────────

void RdpSession::onRemoteFormatList(CliprdrClientContext *cliprdr,
                                     uint32_t fileFormatId, uint32_t textFormatId)
{
    if (fileFormatId) {
        // Server has files on clipboard — request the file list
        m_fileFormatId = fileFormatId;
        m_lastRequestedFormatId = fileFormatId;

        CLIPRDR_FORMAT_DATA_REQUEST request = {};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.requestedFormatId = fileFormatId;
        if (cliprdr->ClientFormatDataRequest)
            cliprdr->ClientFormatDataRequest(cliprdr, &request);
    } else if (textFormatId) {
        // Text only
        m_lastRequestedFormatId = textFormatId;

        CLIPRDR_FORMAT_DATA_REQUEST request = {};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.requestedFormatId = textFormatId;
        if (cliprdr->ClientFormatDataRequest)
            cliprdr->ClientFormatDataRequest(cliprdr, &request);
    }
}

// ── File clipboard: remote → local receive ───────────────────────────

void RdpSession::onRemoteFileListReceived(FILEDESCRIPTORW *descriptors, uint32_t count)
{
    m_recvTempDir = std::make_unique<QTemporaryDir>();
    if (!m_recvTempDir->isValid()) {
        qWarning() << "Failed to create temp dir for received files";
        return;
    }

    m_remoteFiles.clear();
    m_remoteFiles.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        RemoteFileEntry entry;
        // Convert WCHAR filename to QString
        // WCHAR on non-Windows is unsigned short (UTF-16), not wchar_t
        entry.remoteName = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(descriptors[i].cFileName));
        // Resolve local path: replace backslashes with platform separator
        QString localRelative = entry.remoteName;
        localRelative.replace(QLatin1Char('\\'), QDir::separator());
        entry.localPath = m_recvTempDir->path() + QDir::separator() + localRelative;

        entry.isDirectory = (descriptors[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (descriptors[i].dwFlags & FD_FILESIZE) {
            entry.fileSize = (static_cast<uint64_t>(descriptors[i].nFileSizeHigh) << 32)
                           | descriptors[i].nFileSizeLow;
            entry.sizeKnown = true;
        }

        // Create directories immediately
        if (entry.isDirectory) {
            QDir().mkpath(entry.localPath);
        } else {
            // Ensure parent directory exists
            QFileInfo fi(entry.localPath);
            QDir().mkpath(fi.absolutePath());
        }

        m_remoteFiles.push_back(std::move(entry));
    }

    m_recvFileIndex = 0;
    m_recvFileBuffer.clear();
    m_recvFileOffset = 0;
    requestNextFileContents();
}

void RdpSession::requestNextFileContents()
{
    if (!m_cliprdr)
        return;

    // Skip directories and zero-byte files
    while (m_recvFileIndex < static_cast<int>(m_remoteFiles.size())) {
        auto &entry = m_remoteFiles[m_recvFileIndex];
        if (entry.isDirectory) {
            m_recvFileIndex++;
            continue;
        }

        // If size not known yet, request it
        if (!entry.sizeKnown) {
            m_recvWaitingForSize = true;
            CLIPRDR_FILE_CONTENTS_REQUEST request = {};
            request.common.msgType = CB_FILECONTENTS_REQUEST;
            request.streamId = m_nextStreamId++;
            request.listIndex = static_cast<uint32_t>(m_recvFileIndex);
            request.dwFlags = FILECONTENTS_SIZE;
            request.nPositionLow = 0;
            request.nPositionHigh = 0;
            request.cbRequested = 8;
            request.haveClipDataId = FALSE;

            if (m_cliprdr->ClientFileContentsRequest)
                m_cliprdr->ClientFileContentsRequest(m_cliprdr, &request);
            return;
        }

        // Zero-byte file — write empty file and advance
        if (entry.fileSize == 0) {
            QFile f(entry.localPath);
            if (f.open(QIODevice::WriteOnly))
                f.close();
            m_recvFileIndex++;
            continue;
        }

        // Start requesting data chunks
        m_recvWaitingForSize = false;
        m_recvFileBuffer.clear();
        m_recvFileOffset = 0;
        requestFileDataChunk();
        return;
    }

    // All files done
    finishFileReceive();
}

void RdpSession::requestFileDataChunk()
{
    if (!m_cliprdr || m_recvFileIndex >= static_cast<int>(m_remoteFiles.size()))
        return;

    auto &entry = m_remoteFiles[m_recvFileIndex];
    uint64_t remaining = entry.fileSize - m_recvFileOffset;
    if (remaining == 0) {
        writeReceivedFile();
        m_recvFileIndex++;
        requestNextFileContents();
        return;
    }

    uint32_t chunkSize = static_cast<uint32_t>(qMin(remaining, uint64_t(65536)));

    CLIPRDR_FILE_CONTENTS_REQUEST request = {};
    request.common.msgType = CB_FILECONTENTS_REQUEST;
    request.streamId = m_nextStreamId++;
    request.listIndex = static_cast<uint32_t>(m_recvFileIndex);
    request.dwFlags = FILECONTENTS_RANGE;
    request.nPositionLow = static_cast<uint32_t>(m_recvFileOffset & 0xFFFFFFFF);
    request.nPositionHigh = static_cast<uint32_t>(m_recvFileOffset >> 32);
    request.cbRequested = chunkSize;
    request.haveClipDataId = FALSE;

    if (m_cliprdr->ClientFileContentsRequest)
        m_cliprdr->ClientFileContentsRequest(m_cliprdr, &request);
}

void RdpSession::onFileContentsResponse(uint32_t streamId, const BYTE *data,
                                          uint32_t size, bool success)
{
    Q_UNUSED(streamId)

    if (!success) {
        qWarning() << "FileContentsResponse failed for file" << m_recvFileIndex;
        m_recvFileIndex++;
        requestNextFileContents();
        return;
    }

    if (m_recvWaitingForSize) {
        // SIZE response: 8 bytes little-endian uint64
        if (data && size >= 8) {
            uint64_t fileSize = 0;
            memcpy(&fileSize, data, 8);
            if (m_recvFileIndex < static_cast<int>(m_remoteFiles.size())) {
                m_remoteFiles[m_recvFileIndex].fileSize = fileSize;
                m_remoteFiles[m_recvFileIndex].sizeKnown = true;
            }
        }
        m_recvWaitingForSize = false;
        requestNextFileContents();
        return;
    }

    // RANGE response: append data
    if (data && size > 0) {
        m_recvFileBuffer.append(reinterpret_cast<const char *>(data), size);
        m_recvFileOffset += size;
    }

    // Check if file is complete
    if (m_recvFileIndex < static_cast<int>(m_remoteFiles.size())) {
        auto &entry = m_remoteFiles[m_recvFileIndex];
        if (m_recvFileOffset >= entry.fileSize) {
            writeReceivedFile();
            m_recvFileIndex++;
            requestNextFileContents();
        } else {
            requestFileDataChunk();
        }
    }
}

void RdpSession::writeReceivedFile()
{
    if (m_recvFileIndex >= static_cast<int>(m_remoteFiles.size()))
        return;

    auto &entry = m_remoteFiles[m_recvFileIndex];
    QFile file(entry.localPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(m_recvFileBuffer);
        file.close();
    } else {
        qWarning() << "Failed to write received file:" << entry.localPath;
    }
    m_recvFileBuffer.clear();
    m_recvFileOffset = 0;
}

void RdpSession::finishFileReceive()
{
    QStringList paths;
    for (const auto &entry : m_remoteFiles) {
        // Collect only top-level entries (no backslash in remoteName)
        if (!entry.remoteName.contains(QLatin1Char('\\')))
            paths.append(entry.localPath);
    }

    if (!paths.isEmpty())
        emit remoteFilesReceived(paths);
}

// ── File clipboard: local → remote send ──────────────────────────────

void RdpSession::buildLocalFileList(const QList<QUrl> &urls)
{
    m_localFiles.clear();

    for (const QUrl &url : urls) {
        QString path = url.toLocalFile();
        QFileInfo fi(path);
        if (!fi.exists())
            continue;

        LocalFileEntry entry;
        entry.localPath = fi.absoluteFilePath();
        entry.remoteName = fi.fileName();

        if (fi.isDir()) {
            entry.isDirectory = true;
            entry.fileSize = 0;
            m_localFiles.push_back(std::move(entry));
            addDirectoryEntries(fi.absoluteFilePath(), fi.fileName());
        } else {
            entry.isDirectory = false;
            entry.fileSize = static_cast<uint64_t>(fi.size());
            m_localFiles.push_back(std::move(entry));
        }
    }
}

void RdpSession::addDirectoryEntries(const QString &dirPath, const QString &prefix)
{
    QDir dir(dirPath);
    const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        LocalFileEntry entry;
        entry.localPath = fi.absoluteFilePath();
        entry.remoteName = prefix + QLatin1Char('\\') + fi.fileName();

        if (fi.isDir()) {
            entry.isDirectory = true;
            entry.fileSize = 0;
            QString childPrefix = entry.remoteName; // capture before move
            m_localFiles.push_back(std::move(entry));
            addDirectoryEntries(fi.absoluteFilePath(), childPrefix);
        } else {
            entry.isDirectory = false;
            entry.fileSize = static_cast<uint64_t>(fi.size());
            m_localFiles.push_back(std::move(entry));
        }
    }
}

void RdpSession::refreshLocalFilesFromClipboard()
{
    QStringList filePaths;

    // Read clipboard on the main thread (required on macOS for URL types).
    // This blocks the worker thread until the main thread processes the call.
    QMetaObject::invokeMethod(QGuiApplication::instance(), [&filePaths]() {
        QClipboard *cb = QGuiApplication::clipboard();
        if (!cb) return;
        const QMimeData *mimeData = cb->mimeData();
        if (!mimeData || !mimeData->hasUrls()) return;
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile())
                filePaths.append(url.toLocalFile());
        }
    }, Qt::BlockingQueuedConnection);

    if (filePaths.isEmpty())
        return;

    // Feedback loop prevention
    if (m_recvTempDir) {
        bool allInTemp = true;
        for (const QString &path : filePaths) {
            if (!path.startsWith(m_recvTempDir->path())) {
                allInTemp = false;
                break;
            }
        }
        if (allInTemp) return;
    }

    QList<QUrl> urls;
    for (const QString &path : filePaths)
        urls.append(QUrl::fromLocalFile(path));
    buildLocalFileList(urls);
}

void RdpSession::handleFormatDataRequest(CliprdrClientContext *cliprdr, uint32_t requestedFormatId)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;

    if (requestedFormatId == FILE_FORMAT_ID) {
        // Lazy-load: if we haven't built the file list yet, read the clipboard now
        if (m_localFiles.empty())
            refreshLocalFilesFromClipboard();

        if (m_localFiles.empty()) {
            // No files on clipboard — fail this format, server will fall back to text
            response.common.msgFlags = CB_RESPONSE_FAIL;
            response.common.dataLen = 0;
            response.requestedFormatData = nullptr;
            if (cliprdr->ClientFormatDataResponse)
                cliprdr->ClientFormatDataResponse(cliprdr, &response);
            return;
        }
    }

    if (requestedFormatId == FILE_FORMAT_ID && !m_localFiles.empty()) {
        // Build FILEDESCRIPTORW array
        std::vector<FILEDESCRIPTORW> descriptors(m_localFiles.size());
        memset(descriptors.data(), 0, sizeof(FILEDESCRIPTORW) * m_localFiles.size());

        for (size_t i = 0; i < m_localFiles.size(); i++) {
            auto &fd = descriptors[i];
            const auto &lf = m_localFiles[i];

            fd.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME;

            if (lf.isDirectory) {
                fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            } else {
                fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                fd.nFileSizeHigh = static_cast<DWORD>(lf.fileSize >> 32);
                fd.nFileSizeLow = static_cast<DWORD>(lf.fileSize & 0xFFFFFFFF);
            }

            // Set last write time from file
            QFileInfo fi(lf.localPath);
            if (fi.exists()) {
                // Convert QDateTime to FILETIME (100ns intervals since 1601-01-01)
                qint64 msecsSinceEpoch = fi.lastModified().toMSecsSinceEpoch();
                // Windows FILETIME epoch is 1601-01-01, Unix epoch is 1970-01-01
                // Difference is 11644473600 seconds
                uint64_t ft = (static_cast<uint64_t>(msecsSinceEpoch) + 11644473600000ULL) * 10000ULL;
                fd.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(ft >> 32);
                fd.ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
            }

            // Copy filename as UTF-16 (WCHAR = unsigned short, 2 bytes on all platforms)
            const auto *utf16 = lf.remoteName.utf16();
            int nameLen = qMin(lf.remoteName.length(), 259);
            memcpy(fd.cFileName, utf16, nameLen * sizeof(WCHAR));
            fd.cFileName[nameLen] = 0;
        }

        BYTE *formatData = nullptr;
        UINT32 formatDataLen = 0;
        UINT rc = cliprdr_serialize_file_list(descriptors.data(),
                                              static_cast<UINT32>(descriptors.size()),
                                              &formatData, &formatDataLen);
        if (rc == CHANNEL_RC_OK && formatData) {
            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = formatDataLen;
            response.requestedFormatData = formatData;

            if (cliprdr->ClientFormatDataResponse)
                cliprdr->ClientFormatDataResponse(cliprdr, &response);

            free(formatData);
            return;
        }
    } else if (requestedFormatId == 13) { // CF_UNICODETEXT
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString text;

        // If we have local files, provide their paths as text
        if (!m_localFiles.empty()) {
            QStringList paths;
            for (const auto &lf : m_localFiles) {
                if (!lf.remoteName.contains(QLatin1Char('\\')))
                    paths.append(lf.localPath);
            }
            text = paths.join(QLatin1Char('\n'));
        } else {
            text = clipboard ? clipboard->text() : QString();
        }

        if (!text.isEmpty()) {
            const ushort *utf16Data = text.utf16();
            int charCount = text.length() + 1;
            QByteArray utf16(reinterpret_cast<const char *>(utf16Data), charCount * 2);

            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = utf16.size();
            response.requestedFormatData = reinterpret_cast<const BYTE *>(utf16.constData());

            if (cliprdr->ClientFormatDataResponse)
                cliprdr->ClientFormatDataResponse(cliprdr, &response);
            return;
        }
    }

    // Format not available
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    response.requestedFormatData = nullptr;
    if (cliprdr->ClientFormatDataResponse)
        cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

void RdpSession::onServerFileContentsRequest(CliprdrClientContext *cliprdr, uint32_t streamId,
                                               uint32_t listIndex, uint32_t dwFlags,
                                               uint64_t offset, uint32_t cbRequested)
{
    CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
    response.common.msgType = CB_FILECONTENTS_RESPONSE;
    response.streamId = streamId;

    if (listIndex >= m_localFiles.size()) {
        response.common.msgFlags = CB_RESPONSE_FAIL;
        response.cbRequested = 0;
        response.requestedData = nullptr;
        if (cliprdr->ClientFileContentsResponse)
            cliprdr->ClientFileContentsResponse(cliprdr, &response);
        return;
    }

    const auto &lf = m_localFiles[listIndex];

    if (dwFlags & FILECONTENTS_SIZE) {
        // Return 8-byte little-endian file size
        uint64_t size = lf.fileSize;
        QByteArray sizeData(8, 0);
        memcpy(sizeData.data(), &size, 8);

        response.common.msgFlags = CB_RESPONSE_OK;
        response.cbRequested = 8;
        response.requestedData = reinterpret_cast<const BYTE *>(sizeData.constData());

        if (cliprdr->ClientFileContentsResponse)
            cliprdr->ClientFileContentsResponse(cliprdr, &response);
    } else if (dwFlags & FILECONTENTS_RANGE) {
        QFile file(lf.localPath);
        if (!file.open(QIODevice::ReadOnly)) {
            response.common.msgFlags = CB_RESPONSE_FAIL;
            response.cbRequested = 0;
            response.requestedData = nullptr;
            if (cliprdr->ClientFileContentsResponse)
                cliprdr->ClientFileContentsResponse(cliprdr, &response);
            return;
        }

        file.seek(static_cast<qint64>(offset));
        QByteArray data = file.read(cbRequested);
        file.close();

        response.common.msgFlags = CB_RESPONSE_OK;
        response.cbRequested = static_cast<uint32_t>(data.size());
        response.requestedData = reinterpret_cast<const BYTE *>(data.constData());

        if (cliprdr->ClientFileContentsResponse)
            cliprdr->ClientFileContentsResponse(cliprdr, &response);
    }
}
