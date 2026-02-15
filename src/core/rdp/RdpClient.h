#pragma once

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/event.h>
#include <freerdp/client/cliprdr.h>
#include <QRect>

class RdpSession;

// Custom FreeRDP context that holds a pointer back to our RdpSession
struct RdpCustomContext {
    rdpContext context; // Must be first member
    RdpSession *session;
};

// FreeRDP C callback functions
extern "C" {
    BOOL rdp_pre_connect(freerdp *instance);
    BOOL rdp_post_connect(freerdp *instance);
    void rdp_post_disconnect(freerdp *instance);
    BOOL rdp_end_paint(rdpContext *context);
    BOOL rdp_desktop_resize(rdpContext *context);
    BOOL rdp_authenticate(freerdp *instance, char **username, char **password, char **domain);
    BOOL rdp_authenticate_ex(freerdp *instance, char **username, char **password,
                             char **domain, rdp_auth_reason reason);
    DWORD rdp_verify_certificate(freerdp *instance, const char *host, UINT16 port,
                                 const char *common_name, const char *subject,
                                 const char *issuer, const char *fingerprint, DWORD flags);
    DWORD rdp_verify_changed_certificate(freerdp *instance, const char *host, UINT16 port,
                                         const char *common_name, const char *subject,
                                         const char *issuer, const char *new_fingerprint,
                                         const char *old_subject, const char *old_issuer,
                                         const char *old_fingerprint, DWORD flags);

    // Channel loading callback (called by utils_reload_channels inside freerdp_connect)
    BOOL rdp_load_channels(freerdp *instance);

    // PubSub channel event handler
    void rdp_on_channel_connected(void *context, const ChannelConnectedEventArgs *e);
    void rdp_on_channel_disconnected(void *context, const ChannelDisconnectedEventArgs *e);

    // Clipboard channel callbacks
    UINT rdp_cliprdr_monitor_ready(CliprdrClientContext *cliprdr,
                                   const CLIPRDR_MONITOR_READY *monitorReady);
    UINT rdp_cliprdr_server_capabilities(CliprdrClientContext *cliprdr,
                                         const CLIPRDR_CAPABILITIES *capabilities);
    UINT rdp_cliprdr_server_format_list(CliprdrClientContext *cliprdr,
                                        const CLIPRDR_FORMAT_LIST *formatList);
    UINT rdp_cliprdr_server_format_data_response(CliprdrClientContext *cliprdr,
                                                  const CLIPRDR_FORMAT_DATA_RESPONSE *response);
    UINT rdp_cliprdr_server_format_list_response(CliprdrClientContext *cliprdr,
                                                  const CLIPRDR_FORMAT_LIST_RESPONSE *response);
    UINT rdp_cliprdr_server_format_data_request(CliprdrClientContext *cliprdr,
                                                 const CLIPRDR_FORMAT_DATA_REQUEST *request);
    UINT rdp_cliprdr_server_file_contents_request(CliprdrClientContext *cliprdr,
                                                   const CLIPRDR_FILE_CONTENTS_REQUEST *request);
    UINT rdp_cliprdr_server_file_contents_response(CliprdrClientContext *cliprdr,
                                                    const CLIPRDR_FILE_CONTENTS_RESPONSE *response);
}

// Scancode mapping: native keycode → RDP scancode
struct RdpScancode {
    uint16_t code;
    bool extended;
};

#ifdef Q_OS_MACOS
RdpScancode macKeyToRdpScancode(int macKeyCode);
#endif

// Platform-independent: is this Qt key an extended key in RDP terms?
bool isRdpExtendedKey(int qtKey);
