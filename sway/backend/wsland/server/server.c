#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <wlr/util/log.h>
#include <linux/vm_sockets.h>

#include "sway/backend/wsland/wsland.h"
#include "sway/backend/wsland/server.h"


static int create_vsock_fd(const int port) {
    struct sockaddr_vm socket_address;

    const int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0) {
        wlr_log(WLR_INFO, "fail to create vsocket");
        return -1;
    }

    const int bufferSize = 65536;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wlr_log(WLR_INFO, "fail to setsockopt SO_SNDBUF");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wlr_log(WLR_INFO, "fail to setsockopt SO_RCVBUF");
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));

    socket_address.svm_family = AF_VSOCK;
    socket_address.svm_cid = VMADDR_CID_ANY;
    socket_address.svm_port = port;

    const socklen_t socket_addr_size = sizeof(socket_address);

    if (bind(socket_fd, (const struct sockaddr *) &socket_address, socket_addr_size) < 0) {
        wlr_log(WLR_INFO, "fail to bind socket to address socket");
        close(socket_fd);
        return -2;
    }

    const int status = listen(socket_fd, 1);

    if (status != 0) {
        wlr_log(WLR_INFO, "fail to listen on socket");
        close(socket_fd);
        return -4;
    }
    return socket_fd;
}

static int use_vsock_fd(const int port) {
    char *fd_str = getenv("USE_VSOCK");
    if (!fd_str) {
        return -1;
    }

    int fd;
    if (strlen(fd_str) != 0) {
        fd = atoi(fd_str); // NOLINT(*-err34-c)
        wlr_log(WLR_INFO, "using external fd for incoming connections: %d", fd);
        if (fd == 0) {
            fd = -1;
        }
    } else {
        fd = create_vsock_fd(port);
        wlr_log(WLR_INFO, "created vsock for external connections: %d", fd);
    }
    return fd;
}

static int wlr_wsland_server_activity(int _, uint32_t mask, void *data) {
    freerdp_listener *listener = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    if (!listener->CheckFileDescriptor(listener)) {
        wlr_log(WLR_ERROR, "failed to check freerdp file descriptor");
        return -1;
    }
    return 0;
}

struct wl_event_source * wlr_wsland_handle_dispatch_event_source(struct wlr_wsland_backend *backend, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data) {
    return wl_event_loop_add_fd(backend->event_loop, fd, mask, func, data);
}

struct wlr_wsland_server * wlr_wsland_server_create(struct wlr_wsland_backend *backend) {
    struct wlr_wsland_server *server = calloc(1, sizeof(*server));
    if (!server) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_wsland_server");
        return NULL;
    }

    server->address = "0.0.0.0";
    server->port = 3389;

    const char *address = getenv("WLR_WSLAND_SERVER_ADDR");
    if (address) {
        server->address = strdup(address);
    }

    const char *temp_port = getenv("WLR_WSLAND_SERVER_PORT");
    if (temp_port) {
        char *end_ptr;
        int port = (int)strtol(temp_port, &end_ptr, 10);
        if (*end_ptr || port <= 0 || port > 65535) {
            fprintf(stderr, "expected env [ WLR_WSLAND_SERVER_PORT ] to be a positive integer less or equal to 65535\n");
            goto create_failed;
        }
        server->port = port;
    }

    server->listener = freerdp_listener_new();
    if (!server->listener) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp_listener_new");
        goto create_failed;
    }

    server->listener->param4 = server;
    server->listener->PeerAccepted = wlr_wsland_server_incoming_client;

    int vsock_fd = use_vsock_fd(server->port);
    wlr_wsland_server_generate_crypto(server);

    if (vsock_fd > 0) {
        if (!server->listener->OpenFromSocket(server->listener, vsock_fd)) {
            wlr_log(WLR_ERROR, "failed to invoke freerdp OpenFromSocket [ fd: %d ]", vsock_fd);
            goto create_failed;
        }
    } else {
        if (!server->listener->Open(server->listener, server->address, server->port)) {
            wlr_log(WLR_ERROR, "failed to invoke freerdp Open");
            goto create_failed;
        }
    }

    HANDLE handles[WS_MAX_FDS] = { 0 };
    const int count = (int) server->listener->GetEventHandles(server->listener, handles, WS_MAX_FDS);
    if (!count) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp GetFileDescriptor");
        goto create_failed;
    }

    int i;
    for (i = 0; i < count; ++i) {
        const int fd = GetEventFileDescriptor(handles[i]);
        server->sources[i] = wlr_wsland_handle_dispatch_event_source(
            backend, fd, WL_EVENT_READABLE, wlr_wsland_server_activity, server->listener
        );
    }

    for (; i < WS_MAX_FDS; ++i) {
        server->sources[i] = 0;
    }

    server->backend = backend;
    wlr_ws_shared_memory_config(server);
    wl_list_init(&server->monitors);
    return server;

create_failed:
    wlr_wsland_server_destroy(server);
    return NULL;
}

void wlr_wsland_server_destroy(struct wlr_wsland_server *server) {
    if (server) {
        if (server->client) {
            freerdp_peer *peer = server->client->peer;
            freerdp_peer_context_free(peer);
            freerdp_peer_free(peer);
        }
        free(server);
    }
}

void wlr_wsland_server_notify(void) {
    struct sockaddr_un addr = {0};

    char *socket_path = getenv("WSLGD_NOTIFY_SOCKET");
    if (!socket_path) {
        return;
    }

    int socket_fd = socket(PF_LOCAL, SOCK_SEQPACKET, 0);
    if (socket_fd < 0) {
        return;
    }

    addr.sun_family = AF_LOCAL;
    socklen_t name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) + 1;
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + name_size;

    int fd = connect(socket_fd, (struct sockaddr *)&addr, size);
    if (fd < 0) {
        goto close_socket_fd;
    }

    close(fd);

    close_socket_fd:
        close(socket_fd);
}