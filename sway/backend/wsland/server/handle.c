#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <sys/eventfd.h>
#include <cairo/cairo.h>
#include <wlr/util/log.h>
#include <wayland-client-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wayland-client-protocol.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_relative_pointer_v1.h>

#include "sway/backend/wsland/wsland.h"
#include "sway/backend/wsland/server.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"


static int window_id = 0, surface_id = 0, buffer_id = 0, pool_id = 0;

static const struct wlr_pointer_impl wsland_pointer_impl = {
    .name = "wsland-pointer",
};

static const struct wlr_keyboard_impl wsland_keyboard_impl = {
    .name = "wsland-keyboard",
};

struct wlr_wsland_pointer * wlr_wsc_pointer_init(struct wlr_wsland_client *client) {
    struct wlr_wsland_pointer *pointer = calloc(1, sizeof(*pointer));
    if (!pointer) {
        wlr_log(WLR_ERROR, "calloc failed for wsland_pointer");
        return NULL;
    }
    wlr_pointer_init(&pointer->pointer, &wsland_pointer_impl, wsland_pointer_impl.name);

    pointer->pointer.base.data = pointer;
    wl_signal_emit_mutable(&client->server->backend->backend.events.new_input, &pointer->pointer.base);
    pointer->client = client;
    return pointer;
}

struct wlr_wsland_keyboard * wlr_wsc_keyboard_init(struct wlr_wsland_client *client) {
    struct wlr_wsland_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wlr_log(WLR_ERROR, "calloc failed for wsland_keyboard");
        return NULL;
    }
    wlr_keyboard_init(&keyboard->keyboard, &wsland_keyboard_impl, wsland_keyboard_impl.name);

    struct xkb_rule_names rules = {0};
    wlr_wsc_fetch_keyboard_layout(client, &rules);

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    struct sway_seat *seat = input_manager_get_default_seat();

    wlr_keyboard_set_keymap(&keyboard->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(&keyboard->keyboard, 25, 600);
    wlr_seat_set_keyboard(seat->wlr_seat, &keyboard->keyboard);

    wl_signal_emit_mutable(&client->server->backend->backend.events.new_input, &keyboard->keyboard.base);
    keyboard->client = client;
    return keyboard;
}

void wlr_wsc_event_handle(struct wlr_wsland_client *client) {
    if ((client->flags & WSLAND_SERVER_CLIENT_ACTIVATED) == 0) { return; }

    if (!wl_list_empty(&client->event.output)) {
        struct wsc_output_change_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.output, link) {
            wl_list_remove(&event->link);

            if (!wlr_box_equal(&client->current_box, &client->monitor.now)) {
                client->flags &= ~WSLAND_SERVER_OUTPUT_ENABLED;
                if (client->current_output) wlr_output_destroy(client->current_output);

                client->current_box = client->monitor.now;
                client->current_output = wlr_wsland_add_output(&client->server->backend->backend, client->current_box.width, client->current_box.height);
                wlr_wsc_output_window_update(client);
                client->flags |= WSLAND_SERVER_OUTPUT_ENABLED;
            }
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.motion)) {
        struct wsc_mouse_motion_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.motion, link) {
            wl_list_remove(&event->link);

            wl_signal_emit_mutable(&client->pointer->pointer.events.motion_absolute, &event->source);
            wl_signal_emit_mutable(&client->pointer->pointer.events.frame, &client->pointer->pointer);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.button)) {
        struct wsc_mouse_button_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.button, link) {
            wl_list_remove(&event->link);

            wlr_pointer_notify_button(&client->pointer->pointer, &event->source);
            wl_signal_emit_mutable(&client->pointer->pointer.events.frame, &client->pointer->pointer);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.axis)) {
        struct wsc_mouse_axis_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.axis, link) {
            wl_list_remove(&event->link);

            wl_signal_emit_mutable(&client->pointer->pointer.events.axis, &event->source);
            wl_signal_emit_mutable(&client->pointer->pointer.events.frame, &client->pointer->pointer);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.input)) {
        struct wsc_keyboard_input_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.input, link) {
            wl_list_remove(&event->link);

            wlr_keyboard_notify_key(&client->keyboard->keyboard, &event->source);
            free(event);
        }
    }
}

void wlr_wsc_syscommand(bool free_only, void *user_data) {
    struct wlr_wsland_server_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        if (!free_only && (int)data->syscommand.windowId == data->client->wid) {
            switch (data->syscommand.command) {
            case SC_MINIMIZE:
                {
                    WINDOW_ORDER_INFO window_order_info = {0};
                    WINDOW_STATE_ORDER window_state_order = {0};

                    window_order_info.windowId = data->client->wid;
                    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
                    window_state_order.showState = WINDOW_SHOW_MINIMIZED;

                    rdpUpdate *update = data->client->peer->context->update;
                    update->BeginPaint(update->context);
                    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
                    update->EndPaint(update->context);
                }
                break;
            case SC_MAXIMIZE:
                {
                    WINDOW_ORDER_INFO window_order_info = {0};
                    WINDOW_STATE_ORDER window_state_order = {0};

                    window_order_info.windowId = data->client->wid;
                    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
                    window_state_order.showState = WINDOW_SHOW_MAXIMIZED;

                    rdpUpdate *update = data->client->peer->context->update;
                    update->BeginPaint(update->context);
                    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
                    update->EndPaint(update->context);
                }
                break;
            case SC_RESTORE:
                wlr_log(WLR_INFO, "rail client syscommand SC_RESTORE");
                break;
            case SC_CLOSE:
                wlr_log(WLR_INFO, "rail client syscommand SC_CLOSE");
                break;
            case SC_MOVE:
                wlr_log(WLR_INFO, "rail client syscommand SC_MOVE");
                break;
            case SC_SIZE:
                wlr_log(WLR_INFO, "rail client syscommand SC_SIZE");
                break;
            default:
                wlr_log(WLR_INFO, "rail client syscommand %d", data->syscommand.command);
                break;
            }
        }
    }
    free(data);
}

void wlr_wsc_rail_client_sysparam(bool free_only, void *user_data) {
    struct wlr_wsland_server_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        uint32_t p = data->sysparam.params;
        bool is_work_area = p & SPI_MASK_SET_WORK_AREA;
        bool is_taskbar_pos = p & SPI_MASK_TASKBAR_POS;

        if (is_work_area || is_taskbar_pos) {
            RECTANGLE_16 rect = is_work_area ? data->sysparam.workArea : data->sysparam.taskbarPos;
            struct wlr_box area = { .x = rect.left, .y = rect.top, .width = rect.right - rect.left, .height = rect.bottom - rect.top };

            struct wlr_wsland_monitor *wwm;
            wl_list_for_each(wwm, &data->client->server->monitors, link) {
                if (wlr_box_contains_box(&wwm->max, &area)) {
                    if (is_work_area) {
                        wwm->now = area;

                        if (data->client->current_output && wwm->idx == data->client->monitor.idx) {
                            data->client->monitor = *wwm;

                            struct wsc_output_change_event *event = malloc(sizeof(*event));
                            wl_list_insert(&data->client->event.output, &event->link);
                        }
                    } else {
                        wwm->bar = area;
                    }
                    break;
                }
            }
        }
    }
    free(data);
}

void wlr_wsc_rail_client_activate(bool free_only, void *user_data) {
    struct wlr_wsland_server_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only && (int)data->activate.windowId == data->client->wid) {
        if (data->activate.enabled) {
            WINDOW_ORDER_INFO window_order_info = {0};
            WINDOW_STATE_ORDER window_state_order = {0};

            window_order_info.windowId = data->client->wid;
            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
            window_state_order.showState = WINDOW_SHOW;

            rdpUpdate *update = data->client->peer->context->update;
            update->BeginPaint(update->context);
            update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
            update->EndPaint(update->context);
        }
    }
    free(data);
}

void wlr_wsc_appid_response(bool free_only, void *user_data) {
    struct wlr_wsland_server_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only && (int)data->appid.windowId == data->client->wid) {
        RAIL_GET_APPID_RESP_EX resp = {0};
        resp.windowID = data->client->wid;

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "sway-%d", (int)getpid());

        size_t wchar_len = 0;
        WCHAR *wchar_data = ConvertUtf8NToWCharAlloc(buffer, strlen(buffer), &wchar_len);
        memcpy(resp.applicationID, wchar_data, wchar_len);

        RailServerContext *rail_ctx = data->client->rail_ctx;
        rail_ctx->ServerGetAppidRespEx(rail_ctx, &resp);
        free(wchar_data);
    }
    free(data);
}

void wlr_wsland_server_task_dispatch(struct wlr_wsland_client *client, wlr_wsland_server_task_func_t func, struct wlr_wsland_server_task *task) {
    task->client = client;
    task->func = func;

    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_insert(&client->dispatch_tasks, &task->link);
    pthread_mutex_unlock(&client->dispatch_mutex);

    eventfd_write(client->dispatch_fd, 1);
}

void wlr_wsc_output_window_update(struct wlr_wsland_client *client) {
    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = client->wid;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
    window_state_order.showState = WINDOW_SHOW;
    window_state_order.TaskbarButton = false;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
    window_state_order.windowOffsetX = client->monitor.now.x;
    window_state_order.windowOffsetY = client->monitor.now.y;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
    window_state_order.clientAreaHeight = client->monitor.now.height;
    window_state_order.clientAreaWidth = client->monitor.now.width;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
    window_state_order.windowHeight = client->monitor.now.height;
    window_state_order.windowWidth = client->monitor.now.width;

    RECTANGLE_16 window_rect = {
        .left = 0,
        .top = 0,
        .right = client->monitor.now.width,
        .bottom = client->monitor.now.height
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
    window_state_order.numWindowRects = 1;
    window_state_order.windowRects = &window_rect;

    RECTANGLE_16 window_vis = {
        .left = 0,
         .top = 0,
         .right = client->monitor.now.width,
         .bottom = client->monitor.now.height
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
    window_state_order.numVisibilityRects = 1;
    window_state_order.visibilityRects = &window_vis;

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);

    wlr_wsc_buffer_resize(client);
}

static cairo_status_t fetch_icon_data_func(void *closure, unsigned char *data, unsigned int length) {
    FILE *fp = closure;

    size_t n = fread(data, 1, length, fp);
    return n != length ? CAIRO_STATUS_READ_ERROR : CAIRO_STATUS_SUCCESS;
}

static void wlr_wsc_icon_sync(struct wlr_wsland_client *client) {
    FILE *fetch_icon_data = fopen("/usr/share/icons/wsl/linux.png", "r");
    if (!fetch_icon_data) { return; }

    cairo_surface_t *source = cairo_image_surface_create_from_png_stream(fetch_icon_data_func, fetch_icon_data);
    if (cairo_surface_status(source) == CAIRO_STATUS_SUCCESS) {
        int width = cairo_image_surface_get_width(source);
        int height = cairo_image_surface_get_height(source);

        if (width > 0 && height > 0) {
            double tw = 96.0, th = 96.0;
            bool bits_color_allocated = false;
            void *bits_color;
            cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
            cairo_t *cr = cairo_create(target);

            cairo_translate(cr, tw, th);
            cairo_rotate(cr, M_PI);

            cairo_scale(cr, tw / width, th / height);
            cairo_set_source_surface(cr, source, 0, 0);

            cairo_paint(cr);
            cairo_destroy(cr);

            int w = cairo_image_surface_get_width(target);
            int h = cairo_image_surface_get_height(target);
            int s = cairo_image_surface_get_stride(target);

            int stride_color = WSC_ORDER_ICON_ROWLENGTH(w, 32);
            int size_color = stride_color * h;

            if (stride_color != s) {
                size_color = stride_color * h;
                bits_color = malloc(size_color);
                bits_color_allocated = true;
            } else {
                bits_color = cairo_image_surface_get_data(target);
            }

            int stride_mask = WSC_ORDER_ICON_ROWLENGTH(w, 1);
            int size_mask = stride_mask * h;

            void *bits_mask = malloc(size_mask);
            char *src_color = (char *)cairo_image_surface_get_data(target);
            char *dst_color = bits_color;
            char *dst_mask = bits_mask;

            for (int i = 0; i < h; i++) {
                uint32_t *src = (uint32_t *)src_color;
                uint32_t *dst = (uint32_t *)dst_color;
                char *mask = dst_mask;

                for (int j = 0; j < w; j++) {
                    if (dst != src) {
                        *dst = *src;
                    }
                    if (*dst & 0xFF000000) {
                        mask[j / 8] |= 0x80 >> (j % 8); // NOLINT(*-narrowing-conversions)
                    }
                    dst++;
                    src++;
                }
                dst_color += stride_color;
                dst_mask += stride_mask;
                src_color += s;
            }


            WINDOW_ORDER_INFO order_info = {0};
            WINDOW_ICON_ORDER icon_order;
            ICON_INFO icon_info;

            order_info.windowId = client->wid;
            order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_ICON;
            icon_info.cacheEntry = 0xFFFF;
            icon_info.cacheId = 0xFF;
            icon_info.bpp = 32;
            icon_info.width = w;
            icon_info.height = h;
            icon_info.cbColorTable = 0;
            icon_info.cbBitsMask = size_mask;
            icon_info.cbBitsColor = size_color;
            icon_info.bitsMask = bits_mask;
            icon_info.colorTable = NULL;
            icon_info.bitsColor = bits_color;
            icon_order.iconInfo = &icon_info;

            rdpUpdate *update = client->peer->context->update;
            update->BeginPaint(update->context);
            update->window->WindowIcon(update->context, &order_info, &icon_order);
            update->EndPaint(update->context);

            free(bits_mask);
            if (bits_color_allocated) { free(bits_color); }
            cairo_surface_destroy(target);
        }
    }
    cairo_surface_destroy(source);
    pclose(fetch_icon_data);
}

void wlr_wsc_output_window_create(struct wlr_wsland_client *client) {
    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    client->wid = ++window_id;
    window_order_info.windowId = client->wid;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
    window_state_order.style = WSC_WINDOW_NORMAL_STYLE;
    window_state_order.extendedStyle = WS_EX_LAYERED;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
    window_state_order.ownerWindowId = WSC_WINDOW_DESKTOP_ID;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
    window_state_order.clientOffsetX = 0;
    window_state_order.clientOffsetY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
    window_state_order.windowClientDeltaX = 0;
    window_state_order.windowClientDeltaY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
    window_state_order.visibleOffsetX = 0;
    window_state_order.visibleOffsetY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
    window_state_order.windowOffsetX = client->monitor.now.x;
    window_state_order.windowOffsetY = client->monitor.now.y;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
    window_state_order.showState = WINDOW_HIDE;
    window_state_order.TaskbarButton = true;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
    window_state_order.clientAreaWidth = client->monitor.now.width;
    window_state_order.clientAreaHeight = client->monitor.now.height;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
    window_state_order.windowWidth = client->monitor.now.width;
    window_state_order.windowHeight = client->monitor.now.height;

    RECTANGLE_16 window_rect = {
        .left = 0,
        .top = 0,
        .right = client->monitor.now.width,
        .bottom = client->monitor.now.height
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
    window_state_order.numWindowRects = 1;
    window_state_order.windowRects = &window_rect;

    RECTANGLE_16 window_vis = {
        .left = 0,
         .top = 0,
         .right = client->monitor.now.width,
         .bottom = client->monitor.now.height,
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
    window_state_order.numVisibilityRects = 1;
    window_state_order.visibilityRects = &window_vis;

    RAIL_UNICODE_STRING backend_window_title = {.length = 0, .string = NULL};
    if (utf8_string_to_rail_string("sway", &backend_window_title)) {
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
        window_state_order.titleInfo = backend_window_title;
    }

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);

    if (backend_window_title.string) {
        free(backend_window_title.string);
    }

    wlr_wsc_icon_sync(client);

    {
        RAIL_MINMAXINFO_ORDER minmax_order = {0};
        minmax_order.windowId = client->wid;
        minmax_order.maxPosX = 0;
        minmax_order.maxPosY = 0;
        minmax_order.maxWidth = (int16_t)client->monitor.now.width;
        minmax_order.maxHeight = (int16_t)client->monitor.now.height;
        minmax_order.maxTrackWidth = (int16_t)client->monitor.now.width;
        minmax_order.maxTrackHeight = (int16_t)client->monitor.now.height;
        minmax_order.minTrackHeight = 0;
        minmax_order.minTrackWidth = 0;
        client->rail_ctx->ServerMinMaxInfo(client->rail_ctx, &minmax_order);
    }
}

BOOL wlr_wsland_server_adjust_monitor_layout(struct wlr_wsland_client *client, int count, rdpMonitor *monitors) {
    struct wlr_wsland_backend *backend = client->server->backend;
    client->flags &= ~WSLAND_SERVER_OUTPUT_ENABLED;

    {
        struct wlr_wsland_monitor *wwm, *temp2;
        wl_list_for_each_safe(wwm, temp2, &client->server->monitors, link) {
            wl_list_remove(&wwm->link);
            free(wwm);
        }

        // ReSharper disable once CppUnreachableCode
        if (client->current_output) {
            client->monitor = (struct wlr_wsland_monitor) {0};
            client->current_output = NULL;
        }
    }

    // ReSharper disable once CppUnreachableCode
    pixman_region32_t r;
    pixman_region32_init(&r);
    for (int idx = 0; idx < count; idx++) {
        rdpMonitor *monitor = &monitors[idx];
        struct wlr_wsland_monitor *wwm = calloc(1, sizeof(*wwm));
        wwm->idx = idx + 1;
        wwm->primary = monitor->is_primary;
        wwm->scale_desktop = (int)monitor->attributes.desktopScaleFactor;
        wwm->scale_device = (int)monitor->attributes.deviceScaleFactor;
        wwm->phys_height = (int)monitor->attributes.physicalHeight;
        wwm->phys_width = (int)monitor->attributes.physicalWidth;
        wwm->max.height = monitor->height;
        wwm->max.width = monitor->width;
        wwm->max.y = monitor->y;
        wwm->max.x = monitor->x;
        wwm->now = wwm->max;

        wl_list_init(&wwm->link);
        wl_list_insert(&client->server->monitors, &wwm->link);
        pixman_region32_union_rect(&r, &r, wwm->max.x, wwm->max.y, wwm->max.width, wwm->max.height);
    }

    struct wlr_wsland_monitor *wwm, *temp;
    wl_list_for_each(temp, &client->server->monitors, link) {
        if (!wwm) {
            wwm = temp;
        }

        if (temp->primary) {
            wwm = temp;
            break;
        }
    }

    assert(wwm);
    client->monitor = *wwm;

    struct wsc_output_change_event *event = malloc(sizeof(*event));
    wl_list_insert(&client->event.output, &event->link);

    pixman_box32_t *pb = pixman_region32_extents(&r);
    client->server->area = (struct wlr_box) { .x = pb->x1, .y = pb->y1, .width = pb->x2 - pb->x1, .height = pb->y2 - pb->y1 };
    pixman_region32_fini(&r);
    return TRUE;
}

void wlr_wsc_output_cursor_commit(struct wlr_wsland_client *client, struct wlr_output *output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
    struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer, buffer);
    if (!texture) {
        return;
    }

    struct wlr_buffer *cursor_buffer = wlr_allocator_create_buffer(
        output->allocator, buffer->width, buffer->height,
        wlr_drm_format_set_get(&client->server->backend->formats, DRM_FORMAT_ARGB8888)
    );

    struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(output->renderer, cursor_buffer, NULL);
    wlr_render_pass_add_texture(render_pass, &(const struct wlr_render_texture_options) {
        .texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180,
    });

    if (wlr_render_pass_submit(render_pass)) {
        int cursor_bpp = 4;
        int width = cursor_buffer->width;
        int height = cursor_buffer->height;

        int cursor_stride = width * cursor_bpp;
        int cursor_size = cursor_stride * height;
        uint8_t *data = malloc(cursor_size);

        struct wlr_texture *cursor_texture = wlr_texture_from_buffer(output->renderer, cursor_buffer);
        if (cursor_texture && wlr_texture_read_pixels(cursor_texture, &(const struct wlr_texture_read_pixels_options) {
            .data = data, .src_box = { .x = 0, .y = 0, .width = width, .height = height }, .stride = cursor_stride,
            .format = DRM_FORMAT_ARGB8888,
        })) {
            POINTER_LARGE_UPDATE pointerUpdate;
            pointerUpdate.xorBpp = cursor_bpp * 8;
            pointerUpdate.cacheIndex = 0;
            pointerUpdate.hotSpotX = hotspot_x;
            pointerUpdate.hotSpotY = hotspot_y;
            pointerUpdate.height = height,
            pointerUpdate.width = width,
            pointerUpdate.lengthXorMask = cursor_size;
            pointerUpdate.xorMaskData = data;
            pointerUpdate.lengthAndMask = 0;
            pointerUpdate.andMaskData = NULL;

            client->peer->context->update->BeginPaint(client->peer->context->update->context);
            client->peer->context->update->pointer->PointerLarge(client->peer->context->update->context, &pointerUpdate);
            client->peer->context->update->EndPaint(client->peer->context->update->context);
        }
        free(data);
        wlr_texture_destroy(cursor_texture);
        wlr_buffer_drop(cursor_buffer);
    }
    wlr_texture_destroy(texture);
}

static void wait_render_pending_complete(struct wlr_wsland_client *client) {
    int waitRetry = 0;
    while (client->render.pending || client->current_fid != client->acknowledged_fid) {
        if (++waitRetry > 1000) {
            break;
        }
        usleep(10000);
        client->peer->CheckFileDescriptor(client->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
        wlr_log(WLR_INFO, "wait client render complete: %d", waitRetry);
    }
}

void wlr_wsc_output_commit(struct wlr_wsland_client *client, struct wlr_output *output, const struct wlr_output_state *state) {
    if (pixman_region32_not_empty(&state->damage)) {
        RDPGFX_START_FRAME_PDU start_frame = {0};
        bool buffer_opaque = wlr_buffer_is_opaque(state->buffer);
        struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer, state->buffer);
        if (!texture) { goto render_release; }

        pixman_box32_t *pb = pixman_region32_extents(&state->damage);
        struct wlr_box damage = { .x = pb->x1, .y = pb->y1, .width = pb->x2 - pb->x1, .height = pb->y2 - pb->y1, };
        if (wlr_box_empty(&damage)) { goto render_release; }

        int tw = client->monitor.now.width;
        int th = client->monitor.now.height;
        int damage_bpp = 4;

        if (client->server->use_gfxredir) {
            if (!client->render.gfx_buffer) {
                goto render_release;
            }

            int damage_stride = tw * damage_bpp;
            int start_offset = damage.x * damage_bpp + damage.y * damage_stride;
            BYTE *data = (BYTE *)client->render.gfx_buffer + start_offset;

            if (wlr_texture_read_pixels(
                    texture, &(const struct wlr_texture_read_pixels_options){
                        .data = data, .src_box = damage, .stride = damage_stride,
                        .format = DRM_FORMAT_ARGB8888,
                    }
            )) {
                GFXREDIR_PRESENT_BUFFER_PDU present_buffer_pdu;
                present_buffer_pdu.timestamp = 0;
                present_buffer_pdu.orientation = 0;
                present_buffer_pdu.windowId = client->wid;
                present_buffer_pdu.bufferId = client->bid;
                present_buffer_pdu.presentId = ++client->current_fid;
                present_buffer_pdu.targetHeight = th;
                present_buffer_pdu.targetWidth = tw;
                present_buffer_pdu.dirtyRect.left = damage.x;
                present_buffer_pdu.dirtyRect.top = damage.y;
                present_buffer_pdu.dirtyRect.width = damage.width;
                present_buffer_pdu.dirtyRect.height = damage.height;
                if (buffer_opaque) {
                    RECTANGLE_32 opaque_rect = {
                        .left = damage.x, .top = damage.y,
                        .width = damage.width, .height = damage.height,
                    };
                    present_buffer_pdu.numOpaqueRects = 1;
                    present_buffer_pdu.opaqueRects = &opaque_rect;
                } else {
                    present_buffer_pdu.numOpaqueRects = 0;
                    present_buffer_pdu.opaqueRects = NULL;
                }
                if (client->gfxredir_ctx->PresentBuffer(client->gfxredir_ctx, &present_buffer_pdu) == 0) {
                    client->render.pending = true;
                }
            }

            goto render_release;
        }

        if (!start_frame.frameId) {
            start_frame.frameId = ++client->current_fid;
            client->rdpgfx_ctx->StartFrame(client->rdpgfx_ctx, &start_frame);
        }

        int damage_stride = damage.width * damage_bpp;
        int damage_size = damage.height * damage_stride;
        uint8_t *data = malloc(damage_size);

        if (wlr_texture_read_pixels(
            texture, &(const struct wlr_texture_read_pixels_options){
                .data = data, .src_box = damage, .stride = damage_stride,
                .format = DRM_FORMAT_ARGB8888,
            }
        )) {
            RDPGFX_SURFACE_COMMAND surface_command;
            surface_command.surfaceId = client->sid;
            surface_command.format = PIXEL_FORMAT_BGRA32;
            surface_command.left = damage.x;
            surface_command.top = damage.y;
            surface_command.right = damage.x + damage.width;
            surface_command.bottom = damage.y + damage.height;
            surface_command.height = damage.height;
            surface_command.width = damage.width;
            surface_command.contextId = 0;
            surface_command.extra = NULL;

            surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
            surface_command.length = damage_size;
            surface_command.data = &data[0];
            client->rdpgfx_ctx->SurfaceCommand(client->rdpgfx_ctx, &surface_command);
            free(data);
        }

        render_release:
            if (texture) { wlr_texture_destroy(texture); }

        if (start_frame.frameId) {
            RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;
            RDPGFX_END_FRAME_PDU endFrame = { .frameId = start_frame.frameId };
            gfx_ctx->EndFrame(gfx_ctx, &endFrame);
        }
    }
}

void wlr_wsc_buffer_resize(struct wlr_wsland_client *client) {
    if (!client->wid || wlr_box_empty(&client->monitor.now)) {
        return;
    }

    if (client->server->use_gfxredir) {
        if (client->render.gfx_buffer) {
            wait_render_pending_complete(client);

            wlr_log(WLR_INFO, "exec client render buffer destroy");
            wlr_wsc_buffer_destroy(client);
            client->render.gfx_memory.name[0] = '\0';
        }

        int buffer_bpp = 4;
        int width = client->monitor.now.width;
        int height = client->monitor.now.height;
        long page_size = sysconf(_SC_PAGESIZE);
        client->render.gfx_memory.size = width * height * buffer_bpp + (page_size - 1) & ~(page_size - 1);
        if (wlr_ws_shared_memory_new(client->server, &client->render.gfx_memory)) {
            uint16_t section_name[WSC_SHARED_MEMORY_NAME_SIZE + 1];

            for (uint32_t idx = 0; idx < WSC_SHARED_MEMORY_NAME_SIZE; idx++) {
                section_name[idx] = (uint16_t)client->render.gfx_memory.name[idx];
            }
            section_name[WSC_SHARED_MEMORY_NAME_SIZE] = 0;

            GFXREDIR_OPEN_POOL_PDU open_pool_pdu;
            open_pool_pdu.poolId = ++pool_id;
            open_pool_pdu.poolSize = client->render.gfx_memory.size;
            open_pool_pdu.sectionNameLength = WSC_SHARED_MEMORY_NAME_SIZE + 1;
            open_pool_pdu.sectionName = section_name;

            if (client->gfxredir_ctx->OpenPool(client->gfxredir_ctx, &open_pool_pdu) == 0) {
                GFXREDIR_CREATE_BUFFER_PDU create_buffer_pdu;
                create_buffer_pdu.poolId = open_pool_pdu.poolId;
                create_buffer_pdu.bufferId = ++buffer_id;
                create_buffer_pdu.stride = width * buffer_bpp;
                create_buffer_pdu.height = height;
                create_buffer_pdu.width = width;
                create_buffer_pdu.offset = 0;
                create_buffer_pdu.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
                if (client->gfxredir_ctx->CreateBuffer(client->gfxredir_ctx, &create_buffer_pdu) == 0) {
                    client->render.gfx_buffer = client->render.gfx_memory.addr;
                    client->bid = create_buffer_pdu.bufferId;
                    client->pid = create_buffer_pdu.poolId;
                } else {
                    wlr_log(WLR_ERROR, "redir ctx create buffer failed");
                }
            } else {
                wlr_log(WLR_ERROR, "redir ctx open pool failed");
            }

            if (!client->render.gfx_buffer) {
                wlr_wsc_buffer_destroy(client);
            }
        } else {
            wlr_log(WLR_ERROR, "backend shared memory create failed");
        }
    } else {
        uint32_t prev_surface_id = client->sid;

        RDPGFX_CREATE_SURFACE_PDU create_surface;
        create_surface.surfaceId = ++surface_id;
        create_surface.height = client->monitor.now.height;
        create_surface.width = client->monitor.now.width;
        create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
        if (client->rdpgfx_ctx->CreateSurface(client->rdpgfx_ctx, &create_surface) == 0) {
            client->sid = create_surface.surfaceId;
        }

        RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window;
        map_surface_to_window.windowId = client->wid;
        map_surface_to_window.surfaceId = client->sid;
        map_surface_to_window.mappedWidth = client->monitor.now.width;
        map_surface_to_window.mappedHeight = client->monitor.now.height;
        map_surface_to_window.targetWidth = client->monitor.now.width;
        map_surface_to_window.targetHeight = client->monitor.now.height;
        client->rdpgfx_ctx->MapSurfaceToScaledWindow(client->rdpgfx_ctx, &map_surface_to_window);

        if (prev_surface_id) {
            RDPGFX_DELETE_SURFACE_PDU deleteSurface;
            deleteSurface.surfaceId = (uint16_t)prev_surface_id;
            client->rdpgfx_ctx->DeleteSurface(client->rdpgfx_ctx, &deleteSurface);
        }
    }
}

void wlr_wsc_buffer_destroy(struct wlr_wsland_client *client) {
    struct wlr_wsland_server *server = client->server;
    GfxRedirServerContext *redir_ctx = client->gfxredir_ctx;

    if (client->bid) {
        GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer_pdu;
        destroy_buffer_pdu.bufferId = client->bid;
        redir_ctx->DestroyBuffer(redir_ctx, &destroy_buffer_pdu);
        client->bid = 0;
    }

    if (client->pid) {
        GFXREDIR_CLOSE_POOL_PDU close_pool_pdu;
        close_pool_pdu.poolId = client->pid;
        redir_ctx->ClosePool(redir_ctx, &close_pool_pdu);
        client->pid = 0;
    }

    wlr_ws_shared_memory_free(server, &client->render.gfx_memory);
    client->render.gfx_buffer = NULL;
}

void wlr_wsland_server_remove_event_source(struct wl_event_source *source) {
    if (source) {
        wl_event_source_remove(source);
    }
}

void wlr_wsland_server_source_release(struct wlr_wsland_server *server, struct wlr_wsland_pointer *pointer, struct wlr_wsland_keyboard *keyboard) {
    {
        struct wlr_wsland_output *output, *temp;
        wl_list_for_each_safe(output, temp, &server->backend->outputs, link) {
            wlr_output_destroy(&output->output);
        }
    }

    // ReSharper disable once CppUnreachableCode
    if (pointer) {
        wlr_pointer_finish(&pointer->pointer);
        free(pointer);
    }

    if (keyboard) {
        wlr_keyboard_finish(&keyboard->keyboard);
        free(keyboard);
    }
}

bool wlr_ws_shared_memory_new(struct wlr_wsland_server *server, struct wlr_wsc_shared_memory *shared_memory) {
    int fd = -1;
    void *addr = NULL;
    char path[server->shared_memory_mount_point_size + 1 + WSC_SHARED_MEMORY_NAME_SIZE + 1];

    if (shared_memory->size <= 0) {
        goto release;
    }

    if (shared_memory->name[0] == '\0') {
        int fd_uuid = open("/proc/sys/kernel/random/uuid", O_RDONLY);
        if (fd_uuid < 0) {
            goto release;
        }
        if (read(fd_uuid, &shared_memory->name[1], 32 + 4) < 0) {
            goto release;
        }
        close(fd_uuid);
        shared_memory->name[0] = '{';
        shared_memory->name[WSC_SHARED_MEMORY_NAME_SIZE - 1] = '}';
        shared_memory->name[WSC_SHARED_MEMORY_NAME_SIZE] = '\0';
    } else if (strlen(shared_memory->name) != WSC_SHARED_MEMORY_NAME_SIZE ||
        shared_memory->name[0] != '{' ||
        shared_memory->name[WSC_SHARED_MEMORY_NAME_SIZE - 1] != '}' ||
        shared_memory->name[WSC_SHARED_MEMORY_NAME_SIZE] != '\0') {
        goto release;
        }

    strcpy(path, server->shared_memory_mount_point);
    strcat(path, "/");
    strcat(path, shared_memory->name);

    fd = open(path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
    if (fd < 0) {
        goto release;
    }

    if (fallocate(fd, 0, 0, (long)shared_memory->size) < 0) {
        goto release;
    }

    addr = mmap(NULL, shared_memory->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        goto release;
    }

    shared_memory->fd = fd;
    shared_memory->addr = addr;
    return true;

release:
    if (fd > 0) {
        close(fd);
    }
    if (addr) {
        munmap(addr, shared_memory->size);
    }

    shared_memory->fd = -1;
    shared_memory->addr = NULL;
    return false;
}

void wlr_ws_shared_memory_free(struct wlr_wsland_server *server, struct wlr_wsc_shared_memory *shared_memory) {
    if (!shared_memory) { return; }

    if (shared_memory->addr) {
        munmap(shared_memory->addr, shared_memory->size);
        shared_memory->addr = NULL;
    }
    if (shared_memory->fd > 0) {
        close(shared_memory->fd);
        shared_memory->fd = -1;
    }
}

void wlr_ws_shared_memory_config(struct wlr_wsland_server *server) {
    bool use_gfxredir = false;
    char *mount_point = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
    if (mount_point) {
        server->shared_memory_mount_point = mount_point;
        server->shared_memory_mount_point_size = strlen(server->shared_memory_mount_point);
        use_gfxredir = true;
    }

    if (use_gfxredir) {
        use_gfxredir = false;

        for (int idx = 1; idx <=3; ++idx) {
            struct wlr_wsc_shared_memory shared_memory = {0};
            shared_memory.size = sysconf(_SC_PAGESIZE);
            if (wlr_ws_shared_memory_new(server, &shared_memory)) {
                *(uint32_t *)shared_memory.addr = 0x12344321;
                wlr_ws_shared_memory_free(server, &shared_memory);
                use_gfxredir = true;
                break;
            }

            wlr_log(WLR_ERROR, "use gfxredir, but shared memory failed, try: %d", idx);
            sleep(3);
        }

        if (!use_gfxredir) {
            wlr_log(WLR_ERROR, "use gfxredir, but shared memory failed, try exit");
            exit(0);
        }
    }

    server->use_gfxredir = use_gfxredir;
}
