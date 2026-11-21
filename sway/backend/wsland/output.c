#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/util/log.h>

#include "sway/backend/wsland/server.h"
#include "sway/backend/wsland/wsland.h"

static const uint32_t SUPPORTED_OUTPUT_STATE = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;

static struct wlr_wsland_output *wsland_output_from_output(struct wlr_output *wlr_output) {
	assert(wlr_output_is_wsland(wlr_output));
	struct wlr_wsland_output *output = wl_container_of(wlr_output, output, output);
	return output;
}

static void output_update_refresh(struct wlr_wsland_output *output,
		int32_t refresh) {
	if (refresh <= 0) {
		refresh = WSLAND_DEFAULT_REFRESH;
	}

	output->frame_delay = 1000000 / refresh;
}

static bool output_test(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
	}

	if (state->committed & WLR_OUTPUT_STATE_LAYERS) {
		for (size_t i = 0; i < state->layers_len; i++) {
			state->layers[i].accepted = true;
		}
	}

	return true;
}

static bool output_commit(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
	struct wlr_wsland_output *output = wsland_output_from_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		output_update_refresh(output, state->custom_mode.refresh);
	}

	struct wlr_wsland_client *client = output->backend->server->client;
	if (client && state && state->buffer) {
		if (client->flags & WSLAND_SERVER_CLIENT_ACTIVATED && client->flags & WSLAND_SERVER_OUTPUT_ENABLED) {
			if (client->render.pending || client->current_fid - client->acknowledged_fid >= 2) {
				pixman_region32_init((pixman_region32_t *)&state->damage);
				goto update_timer;
			}

			wlr_wsc_output_commit(client, wlr_output, state);
		}
	update_timer:
		wl_event_source_timer_update(output->frame_timer, output->frame_delay);
	}

	return true;
}

static bool output_set_cursor(struct wlr_output *wlr_output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
	struct wlr_wsland_output *output = wsland_output_from_output(wlr_output);

	if (buffer && output->backend->server->client) {
		wlr_wsc_output_cursor_commit(output->backend->server->client, wlr_output, buffer, hotspot_x, hotspot_y);
	}
	return true;
}

static bool output_move_cursor(struct wlr_output *wlr_output, int x, int y) {
	return true;
}

static const struct wlr_drm_format_set * output_get_primary_formats(struct wlr_output *wlr_output, uint32_t buffer_caps) {
	struct wlr_wsland_output *output = wsland_output_from_output(wlr_output);

	return &output->backend->formats;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_wsland_output *output = wsland_output_from_output(wlr_output);

	wlr_output_finish(wlr_output);

	wl_list_remove(&output->link);
	wl_event_source_remove(output->frame_timer);
	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
	.get_primary_formats = output_get_primary_formats
};

bool wlr_output_is_wsland(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(void *data) {
	struct wlr_wsland_output *output = data;
	wlr_output_send_frame(&output->output);
	return 0;
}

struct wlr_output *wlr_wsland_add_output(struct wlr_backend *wlr_backend, unsigned int width, unsigned int height) {
	struct wlr_wsland_backend *backend = wsland_backend_from_backend(wlr_backend);

	struct wlr_wsland_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_wsland_output");
		return NULL;
	}
	output->backend = backend;
	struct wlr_output *wlr_output = &output->output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height, 0);

	wlr_output_init(wlr_output, &backend->backend, &output_impl, backend->event_loop, &state);
	wlr_output_state_finish(&state);

	output_update_refresh(output, 0);

	char name[64];
	snprintf(name, sizeof(name), "WSLAND-%u", 1);
	wlr_output_set_name(wlr_output, name);

	char description[128];
	snprintf(description, sizeof(description), "Wsland output %u", 1);
	wlr_output_set_description(wlr_output, description);

	output->frame_timer = wl_event_loop_add_timer(backend->event_loop, signal_frame, output);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_signal_emit_mutable(&backend->backend.events.new_output, wlr_output);
	}

	return wlr_output;
}