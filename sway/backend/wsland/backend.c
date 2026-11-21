#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include <wlr/interfaces/wlr_output.h>

#include "sway/backend/wsland/wsland.h"
#include "sway/backend/wsland/server.h"


struct wlr_wsland_backend *wsland_backend_from_backend(struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_wsland(wlr_backend));
	struct wlr_wsland_backend *backend = wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_wsland_backend *backend = wsland_backend_from_backend(wlr_backend);
	wlr_log(WLR_INFO, "Starting wsland backend");

	backend->server = wlr_wsland_server_create(backend);
	if (!backend->server) { return false; }

	wlr_wsland_server_notify();
	backend->started = true;
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_wsland_backend *backend = wsland_backend_from_backend(wlr_backend);
	if (!wlr_backend) {
		return;
	}

	wlr_wsland_server_destroy(backend->server);
	wlr_backend_finish(wlr_backend);

	struct wlr_wsland_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
		wlr_output_destroy(&output->output);
	}

	// ReSharper disable once CppUnreachableCode
	wl_list_remove(&backend->event_loop_destroy.link);

	free(backend);
}

void wlr_wsland_switch_next_monitor(struct wlr_backend *wlr_backend) {
	struct wlr_wsland_backend *ws_backend = wsland_backend_from_backend(wlr_backend);

	struct wlr_wsland_client *client = ws_backend->server->client;
	if (!client || (client->flags & WSLAND_SERVER_CLIENT_ACTIVATED) == 0) { return; }

	client->flags &= ~WSLAND_SERVER_OUTPUT_ENABLED;
	struct wl_list *next_link = client->monitor.link.next;
	if (next_link == &client->server->monitors) { next_link = client->server->monitors.next; }
	struct wlr_wsland_monitor *next_monitor = wl_container_of(next_link, next_monitor, link);
	if (next_monitor->idx && next_monitor->idx != client->monitor.idx) {
		client->monitor = *next_monitor;
		if (client->current_output) wlr_output_destroy(client->current_output);

		client->current_box = client->monitor.now;
		client->current_output = wlr_wsland_add_output(&client->server->backend->backend, client->current_box.width, client->current_box.height);
		wlr_wsc_output_window_update(client);
	}
	client->flags |= WSLAND_SERVER_OUTPUT_ENABLED;
}

static int get_drm_fd(struct wlr_backend *backend) {
	return open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = get_drm_fd
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct wlr_wsland_backend *backend = wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_wsland_backend_create(struct wl_event_loop *loop) {
	wlr_log(WLR_INFO, "Creating wsland backend");

	struct wlr_wsland_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_wsland_backend");
		return NULL;
	}

	wlr_backend_init(&backend->backend, &backend_impl);

	backend->backend.buffer_caps = WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;
	wlr_drm_format_set_add(&backend->formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
	wlr_drm_format_set_add(&backend->formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID);

	backend->event_loop = loop;
	wl_list_init(&backend->outputs);

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	backend->backend.features.timeline = true;

	return &backend->backend;
}

bool wlr_backend_is_wsland(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
