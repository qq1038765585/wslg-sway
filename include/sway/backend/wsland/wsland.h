#ifndef BACKEND_WSLAND_H
#define BACKEND_WSLAND_H

#include <wlr/backend/interface.h>

#include "wlr/render/drm_format_set.h"

#define WSLAND_DEFAULT_REFRESH (66 * 1000) // 60 Hz

struct sway_server;

struct wlr_wsland_backend {
	struct wlr_backend backend;
	struct wl_event_loop *event_loop;
	struct wl_listener event_loop_destroy;
	struct wlr_drm_format_set formats;
	struct wl_list outputs;
	bool started;

	struct wlr_wsland_server *server;
};

struct wlr_wsland_output {
	struct wlr_output output;

	struct wl_event_source *frame_timer;
	int frame_delay; // ms

	struct wlr_wsland_backend *backend;
	struct wl_list link;
};

void wlr_wsland_switch_next_monitor(struct wlr_backend *wlr_backend);
struct wlr_wsland_backend *wsland_backend_from_backend(struct wlr_backend *wlr_backend);

struct wlr_backend *wlr_wsland_backend_create(struct wl_event_loop *loop);
struct wlr_output *wlr_wsland_add_output(struct wlr_backend *backend, unsigned int width, unsigned int height);

bool wlr_backend_is_wsland(struct wlr_backend *backend);
bool wlr_output_is_wsland(struct wlr_output *output);
#endif
