#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <freerdp/settings.h>
#include <wlr/types/wlr_buffer.h>
#include <linux/input-event-codes.h>
#include <freerdp/crypto/privatekey.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <freerdp/channels/drdynvc.h>

#include "sway/backend/wsland/server.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);

static int64_t timespec_to_msec(const struct timespec *a) {
    return (int64_t)a->tv_sec * 1000 + a->tv_nsec / 1000000;
}

static void backend_client_channel_destroy(struct wlr_wsland_client *client) {
    if (client->rail_ctx) {
        client->rail_ctx->Stop(client->rail_ctx);

        rail_server_context_free(client->rail_ctx);
        client->rail_ctx = NULL;
    }
    if (client->disp_ctx) {
        client->disp_ctx->Close(client->disp_ctx);

        disp_server_context_free(client->disp_ctx);
        client->disp_ctx = NULL;
    }
    if (client->rdpgfx_ctx) {
        client->rdpgfx_ctx->Close(client->rdpgfx_ctx);

        rdpgfx_server_context_free(client->rdpgfx_ctx);
        client->activation_graphics_completed = false;
        client->rdpgfx_ctx = NULL;
    }
    if (client->clipboard_ctx) {
        wlr_wsc_clipboard_destroy(client);
    }
    if (client->gfxredir_ctx) {
        client->gfxredir_ctx->Close(client->gfxredir_ctx);

        gfxredir_server_context_free(client->gfxredir_ctx);
        client->activation_graphics_redirection_completed = false;
        client->gfxredir_ctx = NULL;
    }
    if (client->drdynvc_ctx) {
        client->drdynvc_ctx->Stop(client->drdynvc_ctx);

        drdynvc_server_context_free(client->drdynvc_ctx);
        client->drdynvc_ctx = NULL;
    }
    if (client->audio_tx) {
        wlr_wsc_audio_tx_destroy(client);
        client->audio_tx = NULL;
    }
    if (client->audio_rx) {
        wlr_wsc_audio_rx_destroy(client);
        client->audio_rx = NULL;
    }
}

static bool backend_client_channel_init(struct wlr_wsland_client *client) {
    freerdp_settings_set_bool(client->peer->context->settings, FreeRDP_DesktopResize, FALSE);

    if (!wlr_wsc_drdynvc_init(client)) {
        goto failed;
    }

    if (!wlr_wsc_rail_init(client)) {
        goto failed;
    }

    if (!wlr_wsc_disp_init(client)) {
        goto failed;
    }

    if (!wlr_wsc_rdpgfx_init(client)) {
        goto failed;
    }

    if (client->server->use_gfxredir) {
        if (!wlr_wsc_gfxredir_init(client)) {
            goto failed;
        }
    }

    if (!wlr_wsc_clipboard_init(client)) {
        goto failed;
    }

    if (!wlr_wsc_audiorx_init(client)) {
        goto failed;
    }

    if (!wlr_wsc_audiotx_init(client)) {
        goto failed;
    }

    /* wait graphics channel (and optionally graphics redir channel) response from client */
    int waitRetry = 0;
    while (!client->activation_graphics_completed || (client->gfxredir_ctx && !client->activation_graphics_redirection_completed)) {
        if (++waitRetry > 10000) { /* timeout after 100 sec. */
            return false;
        }
        usleep(10000); /* wait 0.01 sec. */
        client->peer->CheckFileDescriptor(client->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
    }

    client->current_fid = 0;
    client->acknowledged_fid = 0;
    return true;

failed:
    backend_client_channel_destroy(client);
    return false;
}

static void backend_client_sync_window_state(struct wlr_wsland_client *client) {
    freerdp_peer *peer = client->peer;

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVESECURE,
            .setScreenSaveSecure = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVEACTIVE,
            .setScreenSaveActive = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_ZORDER_SYNC zOrderSync = {
            .windowIdMarker = WSC_WINDOW_MARKER_ID,
        };
        client->rail_ctx->ServerZOrderSync(client->rail_ctx, &zOrderSync);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED | WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        uint32_t windowsIdArray[1] = {0};
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
        };
        windowsIdArray[0] = WSC_WINDOW_MARKER_ID;

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {
            .activeWindowId = WSC_WINDOW_DESKTOP_ID,
            .numWindowIds = 1, .windowIds = (UINT *)&windowsIdArray,
        };

        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    client->activation_rail_completed = true;
}

static BOOL xf_peer_adjust_monitor_layout(freerdp_peer *peer) {
    wlr_log(WLR_INFO, "invoke ==> xf_peer_adjust_monitor_layout");

    struct wlr_wsland_client *client = (struct wlr_wsland_client*)peer->context;
    rdpSettings *settings = client->peer->context->settings;

    int count = (int)freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    if (count > WS_MAX_MONITOR) {
        wlr_log(WLR_ERROR, "freerdp client reports more monitors then expected: [now: %d, max: %d]", count, WS_MAX_MONITOR);
        return FALSE;
    }

    const rdpMonitor *rdp_monitor = freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray);

    rdpMonitor *monitors;
    if (count > 0 && rdp_monitor) {
        monitors = malloc(sizeof(*monitors) * count);
        for (int idx = 0; idx < count; idx++) {
            monitors[idx] = rdp_monitor[idx];
            if (!freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes)) {
                monitors[idx].attributes.physicalWidth = 0;
                monitors[idx].attributes.physicalHeight = 0;
                monitors[idx].attributes.orientation = ORIENTATION_LANDSCAPE;
                monitors[idx].attributes.desktopScaleFactor = 100;
                monitors[idx].attributes.deviceScaleFactor = 100;
            }
        }
    } else {
        count = 1;
        monitors = malloc(sizeof(*monitors) * count);
        monitors[0].x = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopPosX);
        monitors[0].y = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopPosY);
        monitors[0].width = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        monitors[0].height = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        monitors[0].attributes.orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
        monitors[0].attributes.physicalWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalWidth);
        monitors[0].attributes.physicalHeight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalHeight);
        monitors[0].attributes.desktopScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
        monitors[0].attributes.deviceScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);
        monitors[0].orig_screen = 0;
        monitors[0].is_primary = 1;
    }

    BOOL success = wlr_wsland_server_adjust_monitor_layout(client, count, monitors);
    free(monitors);

    return success;
}

static BOOL xf_peer_capabilities(freerdp_peer *peer) {
    wlr_log(WLR_INFO, "invoke ==> xf_peer_capabilities");
    return TRUE;
}

static BOOL xf_peer_post_connect(freerdp_peer *peer) {
    wlr_log(WLR_INFO, "invoke ==> xf_peer_post_connect");
    return TRUE;
}

static BOOL xf_peer_license_callback(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity, BOOL automatic) {
    return TRUE;
}

static BOOL xf_peer_activate(freerdp_peer *peer) {
    wlr_log(WLR_INFO, "invoke ==> xf_peer_activate");

    struct wlr_wsland_client *client = (struct wlr_wsland_client*)peer->context;
    rdpSettings *settings = peer->context->settings;

    if (!freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled)) {
        wlr_log(WLR_ERROR, "freerdp client does not support SurfaceCommands");
        return FALSE;
    }

    if (!freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode)) {
        wlr_log(WLR_ERROR, "freerdp client does not support RemoteApplicationMode");
        return FALSE;
    }

    if (!backend_client_channel_init(client)) {
        return FALSE;
    }

    backend_client_sync_window_state(client);
    if (client->flags & WSLAND_SERVER_CLIENT_ACTIVATED) {
        return TRUE;
    }

    xf_peer_adjust_monitor_layout(peer);
    client->pointer = wlr_wsc_pointer_init(client);
    client->keyboard = wlr_wsc_keyboard_init(client);

    wlr_wsc_output_window_create(client);
    client->flags |= WSLAND_SERVER_CLIENT_ACTIVATED;
    return TRUE;
}

static BOOL xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area) {
    wlr_log(WLR_INFO, "invoke ==> xf_suppress_output");
    return TRUE;
}

static BOOL xf_input_synchronize_event(rdpInput *input, UINT32 flags) {
    struct wlr_wsland_client *client = (struct wlr_wsland_client*)input->context;
    // wlr_log(WLR_INFO, "invoke ==> xf_input_synchronize_event");

    if (!(client->flags & WSLAND_SERVER_OUTPUT_ENABLED)) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int idx = 0; idx < WS_MAX_KEYS; idx++) {
        if (client->server->pressed[idx]) {
            client->server->pressed[idx] = false;

            struct wlr_keyboard_key_event event = {0};
            event.time_msec = timespec_to_msec(&now);
            event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
            event.keycode = idx - 8;
            event.update_state = true;
            wlr_keyboard_notify_key(&client->keyboard->keyboard, &event);
        }
    }

    struct xkb_state *xs;
    if (flags && (xs = client->keyboard->keyboard.xkb_state) != NULL) {
        bool caps_lock = flags & KBD_SYNC_CAPS_LOCK;
        bool xkb_active = xkb_state_mod_name_is_active(xs, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE);

        if (caps_lock != xkb_active) {
            struct wlr_keyboard_key_event event;

            {
                event.time_msec = timespec_to_msec(&now);
                event.state = WL_KEYBOARD_KEY_STATE_PRESSED;
                event.keycode = KEY_CAPSLOCK;
                event.update_state = true;
                wlr_keyboard_notify_key(&client->keyboard->keyboard, &event);
            }

            {
                event.time_msec = timespec_to_msec(&now);
                event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
                event.keycode = KEY_CAPSLOCK;
                event.update_state = true;
                wlr_keyboard_notify_key(&client->keyboard->keyboard, &event);
            }
        }
    }
    return TRUE;
}

static BOOL xf_input_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wlr_wsland_client *client = (struct wlr_wsland_client*)input->context;
    // wlr_log(WLR_INFO, "invoke ==> xf_input_mouse_event");

    if (!(client->flags & WSLAND_SERVER_OUTPUT_ENABLED)) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (flags & PTR_FLAGS_MOVE) {
        int mx = client->monitor.now.x;
        int my = client->monitor.now.y;
        double mw = client->monitor.now.width;
        double mh = client->monitor.now.height;

        struct wsc_mouse_motion_event *event = malloc(sizeof(*event));
        event->source.pointer = &client->pointer->pointer;
        event->source.time_msec = timespec_to_msec(&now);
        event->source.x = (x - mx + client->server->area.x) / mw;
        event->source.y = (y - my + client->server->area.y) / mh;
        wl_list_insert(&client->event.motion, &event->link);
        return TRUE;
    }

    uint32_t button = 0;
    if (flags & PTR_FLAGS_BUTTON1) {
        button = BTN_LEFT;
    }
    else if (flags & PTR_FLAGS_BUTTON2) {
        button = BTN_RIGHT;
    }
    else if (flags & PTR_FLAGS_BUTTON3) {
        button = BTN_MIDDLE;
    }

    if (button) {
        struct wsc_mouse_button_event *event = malloc(sizeof(*event));
        event->source.state = flags & PTR_FLAGS_DOWN ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        event->source.pointer = &client->pointer->pointer;
        event->source.time_msec = timespec_to_msec(&now);
        event->source.button = button;
        wl_list_insert(&client->event.button, &event->link);
        return TRUE;
    }

    if (flags & PTR_FLAGS_WHEEL || flags & PTR_FLAGS_HWHEEL) {
        enum wl_pointer_axis axis;
        if (flags & PTR_FLAGS_WHEEL) {
            axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        } else {
            axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        }
        int8_t rdp_delta = (int8_t)(flags & 0xFF);
        int wheel_delta = -rdp_delta / WLR_POINTER_AXIS_DISCRETE_STEP;

        double delta = wheel_delta * 15;
        int32_t delta_discrete = wheel_delta * WLR_POINTER_AXIS_DISCRETE_STEP;

        struct wsc_mouse_axis_event *event = malloc(sizeof(*event));
        event->source.pointer = &client->pointer->pointer;
        event->source.time_msec = timespec_to_msec(&now);
        event->source.source = WL_POINTER_AXIS_SOURCE_WHEEL;
        event->source.delta_discrete = delta_discrete;
        event->source.orientation = axis;
        event->source.delta = delta;
        wl_list_insert(&client->event.axis, &event->link);
        return TRUE;
    }

    return TRUE;
}

static BOOL xf_input_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    wlr_log(WLR_INFO, "invoke ==> xf_input_extended_mouse_event");
    return TRUE;
}

static BOOL xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
    struct wlr_wsland_client *client = (struct wlr_wsland_client*)input->context;
    // wlr_log(WLR_INFO, "invoke ==> xf_input_keyboard_event");

    if (!(client->flags & WSLAND_SERVER_OUTPUT_ENABLED)) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool is_release = flags & KBD_FLAGS_RELEASE;
    bool is_extended = flags & KBD_FLAGS_EXTENDED;
    enum wl_keyboard_key_state state = is_release ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED;

    {
        uint32_t full_code = code;
        if (is_extended) {
            full_code |= KBD_FLAGS_EXTENDED;
        }

        uint32_t vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
        if (is_extended) {
            vk_code |= KBDEXT;
        }

        bool notify = false;
        uint32_t scan_code = GetKeycodeFromVirtualKeyCode(vk_code, WINPR_KEYCODE_TYPE_XKB);
        uint32_t key_code = scan_code;

        if (is_release) {
            if (client->server->pressed[key_code]) {
                client->server->pressed[key_code] = false;
                notify = true;
            }
        } else {
            if (!client->server->pressed[key_code]) {
                client->server->pressed[key_code] = true;
                notify = true;
            }
        }

        if (notify) {
            struct wsc_keyboard_input_event *event = malloc(sizeof(*event));
            event->source.time_msec = timespec_to_msec(&now);
            event->source.keycode = key_code - 8;
            event->source.update_state = true;
            event->source.state = state;
            wl_list_insert(&client->event.input, &event->link);
        }
    }

    return TRUE;
}

static BOOL xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code) {
    wlr_log(WLR_INFO, "invoke ==> xf_input_unicode_keyboard_event");
    return TRUE;
}

static int backend_client_context_new(freerdp_peer *peer, struct wlr_wsland_client *client) {
    client->peer = peer;

    client->dispatch_fd = -1;
    wl_list_init(&client->dispatch_tasks);

    struct sway_seat *seat = input_manager_get_default_seat();
    wl_signal_add(&seat->wlr_seat->events.set_selection, &client->clipboard_listener);
    client->clipboard_listener.notify = linux_provide_format;
    return true;
}

static void backend_client_context_free(freerdp_peer *_, struct wlr_wsland_client *client) {
    if (!client) {
        return;
    }

    client->flags &= ~WSLAND_SERVER_CLIENT_ACTIVATED;
    client->flags &= ~WSLAND_SERVER_OUTPUT_ENABLED;

    wl_list_remove(&client->clipboard_listener.link);
    wlr_wsland_server_source_release(client->server, client->pointer, client->keyboard);
    for (int i = 0; i < WS_MAX_FDS + 1; ++i) {
        wlr_wsland_server_remove_event_source(client->sources[i]);
        client->sources[i] = 0;
    }

    if (client->dispatch_event_source) {
        wlr_wsland_server_remove_event_source(client->dispatch_event_source);
        client->dispatch_event_source = NULL;
    }

    if (client->render.gfx_buffer) {
        wlr_wsc_buffer_destroy(client);
    }

    struct wlr_wsland_server_task *task, *t_temp;
    wl_list_for_each_reverse_safe(task, t_temp, &client->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        task->func(true, task);
    }

    // ReSharper disable once CppUnreachableCode
    if (client->dispatch_fd != -1) {
        close(client->dispatch_fd);
        client->dispatch_fd = -1;
    }

    pthread_mutex_destroy(&client->dispatch_mutex);
    backend_client_channel_destroy(client);
    client->server->client = NULL;
}

// ReSharper disable once CppDFAConstantFunctionResult
static int wlr_wsland_server_client_activity(int fd, uint32_t mask, void *data) {
    struct wlr_wsland_client *client = data;
    freerdp_peer *peer = client->peer;

    if (!peer->CheckFileDescriptor(peer)) {
        wlr_log(WLR_ERROR, "Unable to check client file descriptor for %p", (void *) data);
        goto peer_release;
    }

    if (client->vcm) {
        if (!client->peer->activated || !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
            return 0;
        }

        if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm)) {
            wlr_log(WLR_ERROR, "failed to check freerdp wts vc file descriptor for %p", data);
            goto peer_release;
        }

    }

    wlr_wsc_event_handle(client);
    return 0;

peer_release:
    freerdp_peer_context_free(peer);
    freerdp_peer_free(peer);
    return 0;
}

static int wlr_wsc_task_check(int fd, uint32_t mask, void *user_data) {
    struct wlr_wsland_client* client = user_data;

    eventfd_t dummy;
    eventfd_read(client->dispatch_fd, &dummy);

    struct wlr_wsland_server_task *task, *temp;
    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_for_each_reverse_safe(task, temp, &client->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        break;
    }
    pthread_mutex_unlock(&client->dispatch_mutex);
    task->func(false, task);
    return 0;
}

static bool wlr_wsc_task_dispatch_init(struct wlr_wsland_client* client) {
    if (pthread_mutex_init(&client->dispatch_mutex, NULL) == -1) { // NOLINT(*-posix-return)
        goto mutex_failed;
    }

    client->dispatch_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (client->dispatch_fd == -1) {
        goto dispatch_fd_error;
    }

    client->dispatch_event_source = wlr_wsland_handle_dispatch_event_source(
        client->server->backend, client->dispatch_fd, WL_EVENT_READABLE, wlr_wsc_task_check, client
    );

    if (!client->dispatch_event_source) {
        goto dispatch_event_source_error;
    }
    return true;

    dispatch_event_source_error:
        close(client->dispatch_fd);
    client->dispatch_fd = -1;
    dispatch_fd_error:
        pthread_mutex_destroy(&client->dispatch_mutex);
    mutex_failed:
        return false;
}

static bool wlr_wsland_server_client_init(struct wlr_wsland_server *server, freerdp_peer *peer) {
    bool start = false;

    if (server->client) {
        wlr_log(WLR_ERROR, "already a client connection, system current only support single client!");
        return false;
    }

    peer->ContextSize = sizeof(struct wlr_wsland_client);
    peer->ContextNew = (psPeerContextNew)backend_client_context_new;
    peer->ContextFree = (psPeerContextFree)backend_client_context_free;
    freerdp_peer_context_new(peer);

    struct wlr_wsland_client *client = (struct wlr_wsland_client*)peer->context;
    client->server = server;

    rdpSettings *settings = peer->context->settings;
    if (server->key_content && server->cert_content) {
        rdpCertificate *cert = freerdp_certificate_new_from_pem(server->cert_content);
        rdpPrivateKey *key = freerdp_key_new_from_pem(server->key_content);

        if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1) ||
            !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
            freerdp_certificate_free(cert);
            freerdp_key_free(key);
            goto init_failed;
        }
    }
    else {
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE);
    }
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE);

    if (!peer->Initialize(peer)) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer Initialize");
        goto init_failed;
    }
    start = true;

    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_RemoteApplicationSupportLevel, RAIL_LEVEL_SUPPORTED |
        RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED | RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED | RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportErrorInfoPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, TRUE);

    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_MultitransportFlags, 0);

    peer->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;
    peer->Capabilities = xf_peer_capabilities;
    peer->PostConnect = xf_peer_post_connect;
    peer->Activate = xf_peer_activate;

    rdpUpdate *update = peer->context->update;
    update->SuppressOutput = xf_suppress_output;

    rdpInput *input = peer->context->input;
    input->SynchronizeEvent = xf_input_synchronize_event;
    input->MouseEvent = xf_input_mouse_event;
    input->ExtendedMouseEvent = xf_input_extended_mouse_event;
    input->KeyboardEvent = xf_input_keyboard_event;
    input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

    HANDLE handles[WS_MAX_FDS];
    int count = (int)peer->GetEventHandles(peer, handles, WS_MAX_FDS);
    if (!count) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer GetEventHandles");
        goto init_failed;
    }

    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
    client->vcm = WTSOpenServerA((LPSTR)peer->context);
    if (client->vcm) {
        handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
    }
    else {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer WTSOpenServerA");
    }

    int i;
    for (i = 0; i < count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);

        client->sources[i] = wlr_wsland_handle_dispatch_event_source(
            server->backend, fd, WL_EVENT_READABLE, wlr_wsland_server_client_activity, client
        );
    }

    for (; i < WS_MAX_FDS; ++i) {
        client->sources[i] = 0;
    }

    if (!wlr_wsc_task_dispatch_init(client)) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_wsland_server_client_task_dispatch_init");
        goto init_failed;
    }

    if (!server->client) {
        server->client = client;
    }

    wl_list_init(&client->event.output);

    wl_list_init(&client->event.motion);
    wl_list_init(&client->event.button);
    wl_list_init(&client->event.axis);

    wl_list_init(&client->event.input);

    wl_list_init(&client->event.clipboard_response);
    wl_list_init(&client->event.clipboard_request);
    return true;

init_failed:
    if (start) {
        peer->Close(peer);
    }
    return false;
}

BOOL wlr_wsland_server_incoming_client(freerdp_listener *listener, freerdp_peer *peer) {
    struct wlr_wsland_server *server = listener->param4;

    if (!wlr_wsland_server_client_init(server, peer)) {
        wlr_log(WLR_ERROR, "error initializing incoming client");
        return FALSE;
    }
    return TRUE;
}