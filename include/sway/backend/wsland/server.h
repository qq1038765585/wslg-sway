#ifndef BACKEND_WSLAND_SERVER_H
#define BACKEND_WSLAND_SERVER_H

#include <stdatomic.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/gfxredir.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/util/box.h>
#include <pixman.h>

#define WS_MAX_FDS 32
#define WS_MAX_KEYS 256
#define WS_MAX_MONITOR 16
#define WS_CLIPBOARD_MAX_FORMATS 5

#define WSC_WINDOW_MARKER_ID  0xFFFFFFFE
#define WSC_WINDOW_DESKTOP_ID 0xFFFFFFFF
#define WSC_SHARED_MEMORY_NAME_SIZE (32 + 4 + 2)
#define WSC_ORDER_ICON_ROWLENGTH( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

#define WSC_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define WSC_WINDOW_NORMAL_STYLE (WSC_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

#define DISPATCH(C, T, D, CB) {                                             \
    struct wlr_wsland_server_task_data *data = malloc(sizeof(*data));       \
    data->client = C; data->T = *(D);                                       \
    wlr_wsland_server_task_dispatch(C, CB, &data->task);                    \
}

enum wlr_ws_flags {
    WSLAND_SERVER_CLIENT_ACTIVATED = 1 << 0,
    WSLAND_SERVER_OUTPUT_ENABLED = 1 << 1,
};

struct wlr_wsland_monitor {
    int idx;
    bool primary;
    int phys_width, phys_height;
    int scale_desktop, scale_device;
    struct wlr_box now, max, work, bar;
    struct wl_list link;
};

struct wlr_wsc_shared_memory {
    int fd;
    void *addr;
    size_t size;
    char name[WSC_SHARED_MEMORY_NAME_SIZE + 1];
};

struct wlr_wsland_server {
    struct wl_event_source *sources[WS_MAX_FDS];
    bool pressed[WS_MAX_KEYS];

    struct wlr_wsland_client *client;
    struct wl_list monitors;

    int port;
    char *address;

    char *key_content;
    char *cert_content;

    char *rail_shell_name;
    void *rail_shell_context;

    bool use_gfxredir;
    char *shared_memory_mount_point;
    size_t shared_memory_mount_point_size;

    bool enable_window_snap_arrange;
    bool enable_window_shadow_remoting;

    struct wlr_box area;

    freerdp_listener *listener;
    struct wlr_wsland_backend *backend;
};

struct wlr_wsland_client {
    rdpContext ctx;
    struct wl_event_source *sources[WS_MAX_FDS + 1];
    freerdp_peer *peer;

    HANDLE vcm;
    enum wlr_ws_flags flags;

    RailServerContext *rail_ctx;
    DispServerContext *disp_ctx;
    RdpgfxServerContext *rdpgfx_ctx;
    DrdynvcServerContext *drdynvc_ctx;
    GfxRedirServerContext *gfxredir_ctx;
    CliprdrServerContext *clipboard_ctx;

    struct clipboard_response_source *clipboard_source;
    struct clipboard_response_source *clipboard_inflight_source;
    struct wl_listener clipboard_listener;

    atomic_int current_fid;
    atomic_int acknowledged_fid;
    int wid, sid, bid, pid;

    bool handshake_completed;
    bool activation_rail_completed;
    bool activation_graphics_completed;
    bool activation_graphics_redirection_completed;
    bool is_acknowledged_suspended;
    bool mouse_button_swap;

    struct wl_list dispatch_tasks;
    struct wl_event_source *dispatch_event_source;
    pthread_mutex_t dispatch_mutex;
    int dispatch_fd;

    struct client_audio_tx *audio_tx;
    struct client_audio_rx *audio_rx;

    struct wlr_wsland_pointer *pointer;
    struct wlr_wsland_keyboard *keyboard;

    struct {
        uint32_t op_id;
        uint32_t format_ids[WS_CLIPBOARD_MAX_FORMATS];
        size_t count;

        struct clipboard_request_source *request_flight;
        struct clipboard_response_source *response_flight;
    } clipboard;

    struct {
        struct wl_list output;

        struct wl_list motion;
        struct wl_list button;
        struct wl_list axis;

        struct wl_list input;

        struct wl_list clipboard_request;
        struct wl_list clipboard_response;

    } event;

    struct {
        atomic_bool pending;
        struct wlr_wsc_shared_memory gfx_memory;
        void *gfx_buffer;
    } render;

    struct wlr_box current_box;
    struct wlr_output *current_output;
    struct wlr_wsland_monitor monitor;
    struct wlr_wsland_server *server;
};

struct wlr_wsland_keyboard {
    struct wlr_keyboard keyboard;

    struct wl_list link;
    struct wlr_wsland_client *client;
};

struct wlr_wsland_pointer {
    struct wlr_pointer pointer;

    struct wlr_wsland_client *client;
};

struct audio_block_info {
    UINT64 submissionTime;
    UINT64 ackReceivedTime;
    UINT64 ackPlayedTime;
};

struct client_audio_tx {
    rdpsnd_server_context *tx_ctx;

    BOOL audioExitSignal;
    int pulseAudioSinkListenerFd;
    int pulseAudioSinkFd;
    pthread_t pulseAudioSinkThread;
    int bytesPerFrame;
    UINT audioBufferSize;
    BYTE* audioBuffer;
    BYTE lastBlockSent;
    UINT64 lastNetworkLatency;
    UINT64 accumulatedNetworkLatency;
    UINT accumulatedNetworkLatencyCount;
    UINT64 lastRenderedLatency;
    UINT64 accumulatedRenderedLatency;
    UINT accumulatedRenderedLatencyCount;
    struct audio_block_info blockInfo[256];
    int nextValidBlock;
    UINT PAVersion;
    int audioSem;
};

struct client_audio_rx {
    audin_server_context *rx_ctx;

    BOOL audioInExitSignal;
    int pulseAudioSourceListenerFd;
    int pulseAudioSourceFd;
    int closeAudioSourceFd;
    pthread_t pulseAudioSourceThread;
    BOOL isAudioInStreamOpened;
};

typedef void (*wlr_wsland_server_task_func_t)(bool free_only, void *data);

struct wsc_monitor_change_data {
    rdpMonitor *monitors;
    int count;
};

struct wsc_output_change_event {
    struct wl_list link;
};

struct wsc_keyboard_input_event {
    struct wlr_keyboard_key_event source;

    struct wl_list link;
};

struct wsc_mouse_motion_event {
    struct wlr_pointer_motion_absolute_event source;

    struct wl_list link;
};

struct wsc_mouse_button_event {
    struct wlr_pointer_button_event source;

    struct wl_list link;
};

struct wsc_mouse_axis_event {
    struct wlr_pointer_axis_event source;

    struct wl_list link;
};

struct windows_response_data_event {
    struct clipboard_response_source *source;

    uint8_t *data;
    uint32_t length;

    struct wl_event_source *write_source;
};

struct wlr_wsland_server_task {
    wlr_wsland_server_task_func_t func;
    struct wl_list link;

    struct wlr_wsland_client *client;
};

struct wlr_wsland_server_task_data {
    struct wlr_wsland_server_task task;

    union {
        RAIL_ACTIVATE_ORDER activate;
        RAIL_SYSPARAM_ORDER sysparam;
        RAIL_SYSCOMMAND_ORDER syscommand;
        RAIL_GET_APPID_REQ_ORDER appid;

        struct wsc_monitor_change_data change_data;
    };

    struct wlr_wsland_client *client;
};

void handle_next_clipboard_request(struct wlr_wsland_client *client);
void server_client_clipboard_linux_publish(struct wlr_wsland_client *client);

void wlr_wsc_event_handle(struct wlr_wsland_client *client);

void wlr_wsc_syscommand(bool free_only, void *user_data);
void wlr_wsc_rail_client_sysparam(bool free_only, void *user_data);
void wlr_wsc_rail_client_activate(bool free_only, void *user_data);
void wlr_wsc_appid_response(bool free_only, void *user_data);

void wlr_wsland_server_task_dispatch(struct wlr_wsland_client *client, wlr_wsland_server_task_func_t func, struct wlr_wsland_server_task *task);
void linux_provide_format(struct wl_listener *listener, void *user_data);
void wlr_wsc_clipboard_destroy(struct wlr_wsland_client *client);
void wlr_wsc_audio_tx_destroy(struct wlr_wsland_client *client);
void wlr_wsc_audio_rx_destroy(struct wlr_wsland_client *client);

bool wlr_wsc_drdynvc_init(struct wlr_wsland_client *client);
bool wlr_wsc_gfxredir_init(struct wlr_wsland_client *client);
bool wlr_wsc_clipboard_init(struct wlr_wsland_client *client);
bool wlr_wsc_audiotx_init(struct wlr_wsland_client *client);
bool wlr_wsc_audiorx_init(struct wlr_wsland_client *client);
bool wlr_wsc_rdpgfx_init(struct wlr_wsland_client *client);
bool wlr_wsc_rail_init(struct wlr_wsland_client *client);
bool wlr_wsc_disp_init(struct wlr_wsland_client *client);

struct wlr_wsland_pointer * wlr_wsc_pointer_init(struct wlr_wsland_client *client);
struct wlr_wsland_keyboard * wlr_wsc_keyboard_init(struct wlr_wsland_client *client);
void wlr_wsc_fetch_keyboard_layout(struct wlr_wsland_client *client, struct xkb_rule_names *rules);

void wlr_wsc_output_cursor_commit(struct wlr_wsland_client *client, struct wlr_output *output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y);
void wlr_wsc_output_commit(struct wlr_wsland_client *client, struct wlr_output *output, const struct wlr_output_state *state);
void wlr_wsc_output_window_update(struct wlr_wsland_client *client);
void wlr_wsc_output_window_create(struct wlr_wsland_client *client);
void wlr_wsc_buffer_resize(struct wlr_wsland_client *client);
void wlr_wsc_buffer_destroy(struct wlr_wsland_client *client);

void wlr_wsland_server_remove_event_source(struct wl_event_source *source);
void wlr_wsland_server_source_release(struct wlr_wsland_server *server, struct wlr_wsland_pointer *pointer, struct wlr_wsland_keyboard *keyboard);

BOOL wlr_wsland_server_adjust_monitor_layout(struct wlr_wsland_client *client, int count, rdpMonitor *monitors);
BOOL wlr_wsland_server_incoming_client(freerdp_listener *listener, freerdp_peer *peer);
void wlr_wsland_server_generate_crypto(struct wlr_wsland_server *server);

bool wlr_ws_shared_memory_new(struct wlr_wsland_server *server, struct wlr_wsc_shared_memory *shared_memory);
void wlr_ws_shared_memory_free(struct wlr_wsland_server *server, struct wlr_wsc_shared_memory *shared_memory);
void wlr_ws_shared_memory_config(struct wlr_wsland_server *server);

struct wl_event_source * wlr_wsland_handle_dispatch_event_source(struct wlr_wsland_backend *backend, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data);
struct wlr_wsland_server * wlr_wsland_server_create(struct wlr_wsland_backend *backend);
void wlr_wsland_server_destroy(struct wlr_wsland_server *server);
void wlr_wsland_server_notify(void);
#endif // BACKEND_WSLAND_SERVER_H
