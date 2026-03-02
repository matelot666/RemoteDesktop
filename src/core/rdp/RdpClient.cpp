#include "RdpClient.h"
#include "RdpSession.h"
#include <freerdp/codec/color.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/event.h>
#include <freerdp/gdi/gfx.h>
#include <winpr/crt.h>
#include <winpr/wtsapi.h>

#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/shell.h>

#include <QGuiApplication>
#include <QClipboard>
#include <QDebug>

extern "C" {

// Custom addin provider: resolves DVC plugins from the compiled-in static
// entry table.  FreeRDP's default loader (freerdp_load_channel_addin_entry)
// tries dlopen first, which fails for our static build.  By registering this
// provider we intercept the lookup and return the static entry directly.
static PVIRTUALCHANNELENTRY rdp_static_addin_provider(LPCSTR pszName,
                                                       LPCSTR pszSubsystem,
                                                       LPCSTR pszType,
                                                       DWORD dwFlags)
{
    // First check the normal static addin table (handles SVCs)
    auto entry = freerdp_channels_load_static_addin_entry(pszName, pszSubsystem,
                                                           pszType, dwFlags);
    if (entry)
        return entry;

    // For dynamic channels, also check the DVCPluginEntry static table
    if (pszName && (dwFlags & FREERDP_ADDIN_CHANNEL_DYNAMIC)) {
        auto dvcEntry = freerdp_channels_client_find_static_entry("DVCPluginEntry", pszName);
        if (dvcEntry)
            return reinterpret_cast<PVIRTUALCHANNELENTRY>(dvcEntry);
    }

    return nullptr;
}

BOOL rdp_load_channels(freerdp *instance)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(instance->context);
    if (!ctx || !ctx->session)
        return TRUE;

    rdpSettings *settings = instance->context->settings;
    BOOL clipEnabled = freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard);
    BOOL gfxEnabled = freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline);

    // Register our custom addin provider so that drdynvc's internal plugin
    // loader can find DVC plugins (like rdpgfx) from the static entry table
    // instead of failing with dlopen.
    freerdp_register_addin_provider(rdp_static_addin_provider, 0);

    // Channels are statically linked into libfreerdp-client — look them up
    // from the compiled-in entry table (NOT dlopen, which would fail).
    if (clipEnabled) {
        auto fn = reinterpret_cast<PVIRTUALCHANNELENTRYEX>(
            freerdp_channels_client_find_static_entry("VirtualChannelEntryEx", "cliprdr"));
        if (fn)
            freerdp_channels_client_load_ex(instance->context->channels, settings, fn, settings);
    }

    if (gfxEnabled) {
        // Load DRDYNVC static channel (transport for dynamic channels)
        auto fn = reinterpret_cast<PVIRTUALCHANNELENTRYEX>(
            freerdp_channels_client_find_static_entry("VirtualChannelEntryEx", "drdynvc"));
        if (fn) {
            freerdp_channels_client_load_ex(instance->context->channels, settings, fn, settings);
        } else {
            return FALSE;
        }

        // Register RDPGFX as a dynamic channel plugin within DRDYNVC
        const char *gfxParams[] = { "rdpgfx" };
        freerdp_client_add_dynamic_channel(settings, 1, gfxParams);
    }

    return TRUE;
}

BOOL rdp_pre_connect(freerdp *instance)
{
    // Request software GDI rendering
    rdpSettings *settings = instance->context->settings;
    if (!freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE))
        return FALSE;

    return TRUE;
}

// Called by FreeRDP (including gdi_ResetGraphics in the GFX pipeline) when
// the desktop dimensions change.  Resizes the GDI primary buffer and
// updates our RdpSession with the new buffer pointer.
BOOL rdp_desktop_resize(rdpContext *context)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(context);
    rdpGdi *gdi = context->gdi;
    rdpSettings *settings = context->settings;

    UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

    if (!gdi_resize(gdi, width, height))
        return FALSE;

    if (ctx && ctx->session) {
        ctx->session->onPostConnect(gdi->width, gdi->height,
                                    gdi->primary_buffer, gdi->stride);
    }

    return TRUE;
}

// Forward declaration — defined below with PubSub handlers
UINT rdp_gfx_caps_confirm(RdpgfxClientContext *gfx,
                           const RDPGFX_CAPS_CONFIRM_PDU *capsConfirm);

BOOL rdp_post_connect(freerdp *instance)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(instance->context);

    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
        return FALSE;

    // Set update callbacks IMMEDIATELY after gdi_init — gdi_ResetGraphics
    // (called by the GFX pipeline's DRDYNVC thread) asserts DesktopResize
    // is non-NULL.  gdi_init() does NOT set it, so we must do it before
    // anything else that could let the GFX thread fire.
    rdpUpdate *update = instance->context->update;
    update->EndPaint = rdp_end_paint;
    update->DesktopResize = rdp_desktop_resize;

    // If RDPGFX channel connected before post_connect (race with DRDYNVC thread),
    // initialize the GFX pipeline now that GDI and DesktopResize are ready.
    if (ctx->pendingGfx) {
        auto *gfx = static_cast<RdpgfxClientContext *>(ctx->pendingGfx);
        gdi_graphics_pipeline_init(instance->context->gdi, gfx);
        gfx->CapsConfirm = rdp_gfx_caps_confirm;
        ctx->pendingGfx = nullptr;
    }

    rdpGdi *gdi = instance->context->gdi;
    ctx->session->onPostConnect(gdi->width, gdi->height,
                                gdi->primary_buffer, gdi->stride);

    return TRUE;
}

void rdp_post_disconnect(freerdp *instance)
{
    gdi_free(instance);
}

BOOL rdp_end_paint(rdpContext *context)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(context);
    rdpGdi *gdi = context->gdi;

    if (gdi->primary->hdc->hwnd->invalid->null)
        return TRUE;

    int x = gdi->primary->hdc->hwnd->invalid->x;
    int y = gdi->primary->hdc->hwnd->invalid->y;
    int w = gdi->primary->hdc->hwnd->invalid->w;
    int h = gdi->primary->hdc->hwnd->invalid->h;

    ctx->session->onEndPaint(QRect(x, y, w, h));

    gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

static BOOL rdp_provide_credentials(freerdp *instance, char **username, char **password,
                                     char **domain, const char *callbackName)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(instance->context);
    Q_UNUSED(callbackName)
    if (!ctx || !ctx->session)
        return TRUE;

    QString fullUser = ctx->session->username();
    QString pass = ctx->session->password();

    if (!fullUser.isEmpty() && username) {
        QString domainPart, userPart;

        int backslash = fullUser.indexOf(QLatin1Char('\\'));
        if (backslash > 0) {
            domainPart = fullUser.left(backslash);
            userPart = fullUser.mid(backslash + 1);
        } else {
            int at = fullUser.indexOf(QLatin1Char('@'));
            if (at > 0) {
                userPart = fullUser.left(at);
                domainPart = fullUser.mid(at + 1);
            } else {
                userPart = fullUser;
            }
        }

        free(*username);
        *username = _strdup(userPart.toUtf8().constData());

        if (!domainPart.isEmpty() && domain) {
            free(*domain);
            *domain = _strdup(domainPart.toUtf8().constData());
        }
    }
    if (!pass.isEmpty() && password) {
        free(*password);
        *password = _strdup(pass.toUtf8().constData());
    }
    if (domain && !*domain) {
        *domain = _strdup("");
    }

    return TRUE;
}

BOOL rdp_authenticate(freerdp *instance, char **username, char **password, char **domain)
{
    return rdp_provide_credentials(instance, username, password, domain, "rdp_authenticate");
}

BOOL rdp_authenticate_ex(freerdp *instance, char **username, char **password,
                          char **domain, rdp_auth_reason reason)
{
    Q_UNUSED(reason)
    return rdp_provide_credentials(instance, username, password, domain, "rdp_authenticate_ex");
}

DWORD rdp_verify_certificate(freerdp *instance, const char *host, UINT16 port,
                              const char *common_name, const char *subject,
                              const char *issuer, const char *fingerprint, DWORD flags)
{
    Q_UNUSED(instance)
    Q_UNUSED(host)
    Q_UNUSED(port)
    Q_UNUSED(common_name)
    Q_UNUSED(subject)
    Q_UNUSED(issuer)
    Q_UNUSED(fingerprint)
    Q_UNUSED(flags)
    // Accept all certificates for now
    return 1;
}

DWORD rdp_verify_changed_certificate(freerdp *instance, const char *host, UINT16 port,
                                      const char *common_name, const char *subject,
                                      const char *issuer, const char *new_fingerprint,
                                      const char *old_subject, const char *old_issuer,
                                      const char *old_fingerprint, DWORD flags)
{
    Q_UNUSED(instance)
    Q_UNUSED(host)
    Q_UNUSED(port)
    Q_UNUSED(common_name)
    Q_UNUSED(subject)
    Q_UNUSED(issuer)
    Q_UNUSED(new_fingerprint)
    Q_UNUSED(old_subject)
    Q_UNUSED(old_issuer)
    Q_UNUSED(old_fingerprint)
    Q_UNUSED(flags)
    return 1;
}

// ── Clipboard channel callbacks ─────────────────────────────────────

UINT rdp_cliprdr_monitor_ready(CliprdrClientContext *cliprdr,
                               const CLIPRDR_MONITOR_READY *monitorReady)
{
    Q_UNUSED(monitorReady)
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    // Send our capabilities
    CLIPRDR_GENERAL_CAPABILITY_SET generalCaps = {};
    generalCaps.capabilitySetType = CB_CAPSTYPE_GENERAL;
    generalCaps.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    generalCaps.version = CB_CAPS_VERSION_2;
    generalCaps.generalFlags = CB_USE_LONG_FORMAT_NAMES
                             | CB_STREAM_FILECLIP_ENABLED
                             | CB_FILECLIP_NO_FILE_PATHS
                             | CB_HUGE_FILE_SUPPORT_ENABLED;

    CLIPRDR_CAPABILITIES caps = {};
    caps.common.msgType = CB_CLIP_CAPS;
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&generalCaps);

    if (cliprdr->ClientCapabilities)
        cliprdr->ClientCapabilities(cliprdr, &caps);

    // Announce that we have text on the clipboard
    session->sendLocalClipboardToServer();

    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_capabilities(CliprdrClientContext *cliprdr,
                                     const CLIPRDR_CAPABILITIES *capabilities)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session || !capabilities)
        return CHANNEL_RC_OK;

    bool fileClip = false;
    for (UINT32 i = 0; i < capabilities->cCapabilitiesSets; i++) {
        auto *capSet = &capabilities->capabilitySets[i];
        if (capSet->capabilitySetType == CB_CAPSTYPE_GENERAL) {
            auto *generalCaps = reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET *>(capSet);
            if (generalCaps->generalFlags & CB_STREAM_FILECLIP_ENABLED)
                fileClip = true;
        }
    }
    session->onServerSupportsFileClip(fileClip);
    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_format_list(CliprdrClientContext *cliprdr,
                                    const CLIPRDR_FORMAT_LIST *formatList)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    // Acknowledge the format list
    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    if (cliprdr->ClientFormatListResponse)
        cliprdr->ClientFormatListResponse(cliprdr, &response);

    // Scan for FileGroupDescriptorW and text formats
    uint32_t fileFormatId = 0;
    uint32_t textFormatId = 0;
    for (uint32_t i = 0; i < formatList->numFormats; i++) {
        const auto &fmt = formatList->formats[i];
        if (fmt.formatName && strcmp(fmt.formatName, "FileGroupDescriptorW") == 0) {
            fileFormatId = fmt.formatId;
        }
        if (fmt.formatId == 13) // CF_UNICODETEXT
            textFormatId = 13;
        else if (fmt.formatId == 1 && textFormatId == 0) // CF_TEXT
            textFormatId = 1;
    }

    session->onRemoteFormatList(cliprdr, fileFormatId, textFormatId);
    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_format_data_response(CliprdrClientContext *cliprdr,
                                              const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session || !response)
        return CHANNEL_RC_OK;

    if (response->common.msgFlags != CB_RESPONSE_OK)
        return CHANNEL_RC_OK;

    if (!response->requestedFormatData || response->common.dataLen == 0)
        return CHANNEL_RC_OK;

    // Check if this was a file list request
    if (session->isFileFormatResponse()) {
        FILEDESCRIPTORW *descriptors = nullptr;
        UINT32 count = 0;
        UINT rc = cliprdr_parse_file_list(response->requestedFormatData,
                                          response->common.dataLen,
                                          &descriptors, &count);
        if (rc == CHANNEL_RC_OK && descriptors && count > 0) {
            session->onRemoteFileListReceived(descriptors, count);
            free(descriptors);
        }
        return CHANNEL_RC_OK;
    }

    // Text clipboard data
    QString text = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(response->requestedFormatData),
        response->common.dataLen / 2);

    // Remove trailing null characters
    while (!text.isEmpty() && text.back() == QChar('\0'))
        text.chop(1);

    if (!text.isEmpty())
        session->onClipboardDataReceived(text);

    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_format_list_response(CliprdrClientContext *cliprdr,
                                              const CLIPRDR_FORMAT_LIST_RESPONSE *response)
{
    Q_UNUSED(cliprdr)
    Q_UNUSED(response)
    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_format_data_request(CliprdrClientContext *cliprdr,
                                             const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session || !request)
        return CHANNEL_RC_OK;

    session->handleFormatDataRequest(cliprdr, request->requestedFormatId);
    return CHANNEL_RC_OK;
}

// ── File contents callbacks ──────────────────────────────────────────

UINT rdp_cliprdr_server_file_contents_response(CliprdrClientContext *cliprdr,
                                                const CLIPRDR_FILE_CONTENTS_RESPONSE *response)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session || !response)
        return CHANNEL_RC_OK;

    bool success = (response->common.msgFlags & CB_RESPONSE_OK) != 0;
    session->onFileContentsResponse(response->streamId,
                                    response->requestedData,
                                    response->cbRequested,
                                    success);
    return CHANNEL_RC_OK;
}

UINT rdp_cliprdr_server_file_contents_request(CliprdrClientContext *cliprdr,
                                               const CLIPRDR_FILE_CONTENTS_REQUEST *request)
{
    auto *session = static_cast<RdpSession *>(cliprdr->custom);
    if (!session || !request)
        return CHANNEL_RC_OK;

    uint64_t offset = (static_cast<uint64_t>(request->nPositionHigh) << 32) | request->nPositionLow;
    session->onServerFileContentsRequest(cliprdr, request->streamId,
                                          request->listIndex, request->dwFlags,
                                          offset, request->cbRequested);
    return CHANNEL_RC_OK;
}

// ── GFX caps confirm callback ────────────────────────────────────────

UINT rdp_gfx_caps_confirm(RdpgfxClientContext *gfx,
                           const RDPGFX_CAPS_CONFIRM_PDU *capsConfirm)
{
    if (!gfx || !gfx->custom || !capsConfirm)
        return CHANNEL_RC_OK;

    // gdi_graphics_pipeline_init sets gfx->custom = gdi
    auto *gdi = static_cast<rdpGdi *>(gfx->custom);
    if (!gdi || !gdi->context)
        return CHANNEL_RC_OK;

    auto *ctx = reinterpret_cast<RdpCustomContext *>(gdi->context);
    if (ctx && ctx->session)
        ctx->session->setGfxCapsConfirm(capsConfirm->capsSet->version,
                                         capsConfirm->capsSet->flags);
    return CHANNEL_RC_OK;
}

// ── PubSub channel event handlers ───────────────────────────────────

void rdp_on_channel_connected(void *context, const ChannelConnectedEventArgs *e)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(context);
    if (!ctx || !ctx->session || !e)
        return;

    if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0 && e->pInterface) {
        auto *cliprdr = static_cast<CliprdrClientContext *>(e->pInterface);
        cliprdr->custom = ctx->session;
        cliprdr->MonitorReady = rdp_cliprdr_monitor_ready;
        cliprdr->ServerCapabilities = rdp_cliprdr_server_capabilities;
        cliprdr->ServerFormatList = rdp_cliprdr_server_format_list;
        cliprdr->ServerFormatListResponse = rdp_cliprdr_server_format_list_response;
        cliprdr->ServerFormatDataResponse = rdp_cliprdr_server_format_data_response;
        cliprdr->ServerFormatDataRequest = rdp_cliprdr_server_format_data_request;
        cliprdr->ServerFileContentsResponse = rdp_cliprdr_server_file_contents_response;
        cliprdr->ServerFileContentsRequest = rdp_cliprdr_server_file_contents_request;
        ctx->session->setCliprdrContext(cliprdr);
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0 && e->pInterface) {
        auto *gfx = static_cast<RdpgfxClientContext *>(e->pInterface);
        ctx->session->setGfxChannelActive(true);
        rdpGdi *gdi = ctx->context.gdi;
        if (gdi && ctx->context.update && ctx->context.update->DesktopResize) {
            // GDI and DesktopResize ready — init pipeline now
            gdi_graphics_pipeline_init(gdi, gfx);
            gfx->CapsConfirm = rdp_gfx_caps_confirm;
        } else {
            // GDI not ready yet — defer to rdp_post_connect
            ctx->pendingGfx = gfx;
        }
    }

    if (strcmp(e->name, "drdynvc") == 0)
        ctx->session->setDrdynvcActive(true);
}

void rdp_on_channel_disconnected(void *context, const ChannelDisconnectedEventArgs *e)
{
    auto *ctx = reinterpret_cast<RdpCustomContext *>(context);
    if (!ctx || !ctx->session || !e)
        return;

    if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
        ctx->session->setCliprdrContext(nullptr);
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0 && e->pInterface) {
        ctx->session->setGfxChannelActive(false);
        auto *gfx = static_cast<RdpgfxClientContext *>(e->pInterface);
        if (ctx->pendingGfx == gfx) {
            // Pipeline was never initialized (deferred init, connection failed)
            ctx->pendingGfx = nullptr;
        } else {
            rdpGdi *gdi = ctx->context.gdi;
            if (gdi)
                gdi_graphics_pipeline_uninit(gdi, gfx);
        }
    }

    if (strcmp(e->name, "drdynvc") == 0)
        ctx->session->setDrdynvcActive(false);
}

} // extern "C"

bool isRdpExtendedKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_Insert: case Qt::Key_Delete:
    case Qt::Key_Home: case Qt::Key_End:
    case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_Print: case Qt::Key_NumLock:
    case Qt::Key_Meta: // Windows key
        return true;
    default:
        return false;
    }
}

#ifdef Q_OS_MACOS
// macOS virtual keycode → RDP scancode mapping table
// Based on Carbon Events.h kVK_* constants
static const RdpScancode s_macToRdp[128] = {
    {0x1E, false}, // 0x00 kVK_ANSI_A
    {0x1F, false}, // 0x01 kVK_ANSI_S
    {0x20, false}, // 0x02 kVK_ANSI_D
    {0x21, false}, // 0x03 kVK_ANSI_F
    {0x23, false}, // 0x04 kVK_ANSI_H
    {0x22, false}, // 0x05 kVK_ANSI_G
    {0x2C, false}, // 0x06 kVK_ANSI_Z
    {0x2D, false}, // 0x07 kVK_ANSI_X
    {0x2E, false}, // 0x08 kVK_ANSI_C
    {0x2F, false}, // 0x09 kVK_ANSI_V
    {0x2B, false}, // 0x0A kVK_ISO_Section → OEM_5 (# on UK PC, next to left shift on ISO Mac)
    {0x30, false}, // 0x0B kVK_ANSI_B
    {0x10, false}, // 0x0C kVK_ANSI_Q
    {0x11, false}, // 0x0D kVK_ANSI_W
    {0x12, false}, // 0x0E kVK_ANSI_E
    {0x13, false}, // 0x0F kVK_ANSI_R
    {0x15, false}, // 0x10 kVK_ANSI_Y
    {0x14, false}, // 0x11 kVK_ANSI_T
    {0x02, false}, // 0x12 kVK_ANSI_1
    {0x03, false}, // 0x13 kVK_ANSI_2
    {0x04, false}, // 0x14 kVK_ANSI_3
    {0x05, false}, // 0x15 kVK_ANSI_4
    {0x07, false}, // 0x16 kVK_ANSI_6
    {0x06, false}, // 0x17 kVK_ANSI_5
    {0x0D, false}, // 0x18 kVK_ANSI_Equal
    {0x0A, false}, // 0x19 kVK_ANSI_9
    {0x08, false}, // 0x1A kVK_ANSI_7
    {0x0C, false}, // 0x1B kVK_ANSI_Minus
    {0x09, false}, // 0x1C kVK_ANSI_8
    {0x0B, false}, // 0x1D kVK_ANSI_0
    {0x1B, false}, // 0x1E kVK_ANSI_RightBracket
    {0x18, false}, // 0x1F kVK_ANSI_O
    {0x16, false}, // 0x20 kVK_ANSI_U
    {0x1A, false}, // 0x21 kVK_ANSI_LeftBracket
    {0x17, false}, // 0x22 kVK_ANSI_I
    {0x19, false}, // 0x23 kVK_ANSI_P
    {0x1C, false}, // 0x24 kVK_Return
    {0x26, false}, // 0x25 kVK_ANSI_L
    {0x24, false}, // 0x26 kVK_ANSI_J
    {0x28, false}, // 0x27 kVK_ANSI_Quote
    {0x25, false}, // 0x28 kVK_ANSI_K
    {0x27, false}, // 0x29 kVK_ANSI_Semicolon
    {0x56, false}, // 0x2A kVK_ANSI_Backslash → OEM_102 (\ on UK PC, between ' and Enter on ISO Mac)
    {0x33, false}, // 0x2B kVK_ANSI_Comma
    {0x35, false}, // 0x2C kVK_ANSI_Slash
    {0x31, false}, // 0x2D kVK_ANSI_N
    {0x32, false}, // 0x2E kVK_ANSI_M
    {0x34, false}, // 0x2F kVK_ANSI_Period
    {0x0F, false}, // 0x30 kVK_Tab
    {0x39, false}, // 0x31 kVK_Space
    {0x29, false}, // 0x32 kVK_ANSI_Grave
    {0x0E, false}, // 0x33 kVK_Delete (Backspace)
    {0x00, false}, // 0x34 (unused)
    {0x01, false}, // 0x35 kVK_Escape
    {0x5B, true},  // 0x36 kVK_RightCommand → Right Windows
    {0x5B, true},  // 0x37 kVK_Command → Left Windows
    {0x2A, false}, // 0x38 kVK_Shift → Left Shift
    {0x3A, false}, // 0x39 kVK_CapsLock
    {0x38, false}, // 0x3A kVK_Option → Left Alt
    {0x1D, false}, // 0x3B kVK_Control → Left Ctrl
    {0x36, false}, // 0x3C kVK_RightShift
    {0x38, true},  // 0x3D kVK_RightOption → Right Alt
    {0x1D, true},  // 0x3E kVK_RightControl → Right Ctrl
    {0x00, false}, // 0x3F kVK_Function
    {0x57, false}, // 0x40 kVK_F17
    {0x53, false}, // 0x41 kVK_ANSI_KeypadDecimal
    {0x00, false}, // 0x42 (unused)
    {0x37, false}, // 0x43 kVK_ANSI_KeypadMultiply
    {0x00, false}, // 0x44 (unused)
    {0x4E, false}, // 0x45 kVK_ANSI_KeypadPlus
    {0x00, false}, // 0x46 (unused)
    {0x45, false}, // 0x47 kVK_ANSI_KeypadClear → NumLock
    {0x00, false}, // 0x48 kVK_VolumeUp
    {0x00, false}, // 0x49 kVK_VolumeDown
    {0x00, false}, // 0x4A kVK_Mute
    {0x35, false}, // 0x4B kVK_ANSI_KeypadDivide (extended)
    {0x1C, true},  // 0x4C kVK_ANSI_KeypadEnter
    {0x00, false}, // 0x4D (unused)
    {0x4A, false}, // 0x4E kVK_ANSI_KeypadMinus
    {0x58, false}, // 0x4F kVK_F18
    {0x59, false}, // 0x50 kVK_F19
    {0x0D, false}, // 0x51 kVK_ANSI_KeypadEquals
    {0x52, false}, // 0x52 kVK_ANSI_Keypad0
    {0x4F, false}, // 0x53 kVK_ANSI_Keypad1
    {0x50, false}, // 0x54 kVK_ANSI_Keypad2
    {0x51, false}, // 0x55 kVK_ANSI_Keypad3
    {0x4B, false}, // 0x56 kVK_ANSI_Keypad4
    {0x4C, false}, // 0x57 kVK_ANSI_Keypad5
    {0x4D, false}, // 0x58 kVK_ANSI_Keypad6
    {0x47, false}, // 0x59 kVK_ANSI_Keypad7
    {0x5A, false}, // 0x5A kVK_F20
    {0x48, false}, // 0x5B kVK_ANSI_Keypad8
    {0x49, false}, // 0x5C kVK_ANSI_Keypad9
    {0x00, false}, // 0x5D
    {0x00, false}, // 0x5E
    {0x00, false}, // 0x5F
    {0x3B, false}, // 0x60 kVK_F5
    {0x3C, false}, // 0x61 kVK_F6
    {0x3D, false}, // 0x62 kVK_F7
    {0x3E, false}, // 0x63 kVK_F3
    {0x3F, false}, // 0x64 kVK_F8
    {0x40, false}, // 0x65 kVK_F9
    {0x00, false}, // 0x66
    {0x42, false}, // 0x67 kVK_F11
    {0x00, false}, // 0x68
    {0x37, false}, // 0x69 kVK_F13 → PrintScreen
    {0x57, false}, // 0x6A kVK_F16
    {0x46, false}, // 0x6B kVK_F14 → ScrollLock
    {0x00, false}, // 0x6C
    {0x44, false}, // 0x6D kVK_F10
    {0x00, false}, // 0x6E
    {0x43, false}, // 0x6F kVK_F12
    {0x00, false}, // 0x70
    {0x45, true},  // 0x71 kVK_F15 → Pause
    {0x52, true},  // 0x72 kVK_Help → Insert
    {0x47, true},  // 0x73 kVK_Home
    {0x49, true},  // 0x74 kVK_PageUp
    {0x53, true},  // 0x75 kVK_ForwardDelete → Delete
    {0x3E, false}, // 0x76 kVK_F4
    {0x4F, true},  // 0x77 kVK_End
    {0x3C, false}, // 0x78 kVK_F2
    {0x51, true},  // 0x79 kVK_PageDown
    {0x3B, false}, // 0x7A kVK_F1
    {0x4B, true},  // 0x7B kVK_LeftArrow
    {0x4D, true},  // 0x7C kVK_RightArrow
    {0x50, true},  // 0x7D kVK_DownArrow
    {0x48, true},  // 0x7E kVK_UpArrow
    {0x00, false}, // 0x7F
};

RdpScancode macKeyToRdpScancode(int macKeyCode)
{
    if (macKeyCode >= 0 && macKeyCode < 128)
        return s_macToRdp[macKeyCode];
    return {0, false};
}
#endif // Q_OS_MACOS
