#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>

#include "utf8.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "net-tapesoftware-dwl-wm-unstable-v1-protocol.h"

#define DIE(fmt, ...)						\
	do {							\
		cleanup();					\
		fprintf(stderr, fmt "\n", ##__VA_ARGS__);	\
		exit(1);					\
	} while (0)
#define EDIE(fmt, ...)						\
	DIE(fmt ": %s", ##__VA_ARGS__, strerror(errno));

#define MIN(a, b)				\
	((a) < (b) ? (a) : (b))
#define MAX(a, b)				\
	((a) > (b) ? (a) : (b))
#define LENGTH(x)				\
	(sizeof x / sizeof x[0])

#define ARRAY_INIT_CAP 16
#define ARRAY_EXPAND(arr, len, cap, inc)				\
	do {								\
		uint32_t new_len, new_cap;				\
		new_len = (len) + (inc);				\
		if (new_len > (cap)) {					\
			new_cap = new_len * 2;				\
			if (new_cap < ARRAY_INIT_CAP)			\
				new_cap = ARRAY_INIT_CAP;		\
			if (!((arr) = realloc((arr), sizeof(*(arr)) * new_cap))) \
				EDIE("realloc");			\
			(cap) = new_cap;				\
		}							\
		(len) = new_len;					\
	} while (0)
#define ARRAY_APPEND(arr, len, cap, ptr)		\
	do {						\
		ARRAY_EXPAND((arr), (len), (cap), 1);	\
		(ptr) = &(arr)[(len) - 1];		\
	} while (0)

#define PROGRAM "dwlb"
#define VERSION "0.2"
#define USAGE								\
	"usage: dwlb [OPTIONS]\n"					\
	"Ipc\n"								\
	"	-ipc				allow commands to be sent to dwl (dwl must be patched)\n" \
	"	-no-ipc				disable ipc\n"		\
	"Bar Config\n"							\
	"	-hidden				bars will initially be hidden\n" \
	"	-no-hidden			bars will not initially be hidden\n" \
	"	-bottom				bars will initially be drawn at the bottom\n" \
	"	-no-bottom			bars will initially be drawn at the top\n" \
	"	-hide-vacant-tags		do not display empty and inactive tags\n" \
	"	-no-hide-vacant-tags		display empty and inactive tags\n" \
	"	-status-commands		enable in-line commands in status text\n" \
	"	-no-status-commands		disable in-line commands in status text\n" \
	"	-center-title			center title text on bar\n" \
	"	-no-center-title		do not center title text on bar\n" \
	"	-custom-title			do not display window title and treat the area as another status text element; see -title command\n" \
	"	-no-custom-title		display current window title as normal\n" \
	"	-font [FONT]			specify a font\n"	\
	"	-tags [NUMBER] [FIRST]...[LAST]	if ipc is disabled, specify custom tag names\n" \
	"	-vertical-padding [PIXELS]	specify vertical pixel padding above and below text\n" \
	"	-active-fg-color [COLOR]	specify text color of active tags or monitors\n" \
	"	-active-bg-color [COLOR]	specify background color of active tags or monitors\n" \
	"	-inactive-fg-color [COLOR]	specify text color of inactive tags or monitors\n" \
	"	-inactive-fg-color [COLOR]	specify background color of inactive tags or monitors\n" \
	"	-urgent-fg-color [COLOR]	specify text color of urgent tags\n" \
	"	-urgent-bg-color [COLOR]	specify background color of urgent tags\n" \
	"	-scale [BUFFER_SCALE]		specify buffer scale value for integer scaling\n" \
	"Commands\n"							\
	"	-status	[OUTPUT] [TEXT]		set status text\n"	\
	"	-title	[OUTPUT] [TEXT]		set title text, if -custom-title is enabled\n"	\
	"	-show [OUTPUT]			show bar\n"		\
	"	-hide [OUTPUT]			hide bar\n"		\
	"	-toggle-visibility [OUTPUT]	toggle bar visibility\n" \
	"	-set-top [OUTPUT]		draw bar at the top\n"	\
	"	-set-bottom [OUTPUT]		draw bar at the bottom\n" \
	"	-toggle-location [OUTPUT]	toggle bar location\n"	\
	"Other\n"							\
	"	-v				get version information\n" \
	"	-h				view this help text\n"

typedef struct {
	pixman_color_t color;
	bool bg;
	char *start;
} Color;

typedef struct {
	uint32_t btn;
	uint32_t x1;
	uint32_t x2;
	char command[128];
} Button;

typedef struct {
	char text[2048];
	Color *colors;
	uint32_t colors_l, colors_c;
	Button *buttons;
	uint32_t buttons_l, buttons_c;
} CustomText;

typedef struct {
	struct wl_output *wl_output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct zxdg_output_v1 *xdg_output;
	struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor;

	uint32_t registry_name;
	char *xdg_output_name;

	bool configured;
	uint32_t width, height;
	uint32_t textpadding;
	uint32_t stride, bufsize;
	
	uint32_t mtags, ctags, urg, sel;
	char *layout, *window_title;
	uint32_t layout_idx, last_layout_idx;
	CustomText title, status;

	bool hidden, bottom;
	bool redraw;

	struct wl_list link;
} Bar;

typedef struct {
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;
	uint32_t registry_name;

	Bar *bar;
	uint32_t pointer_x, pointer_y;
	uint32_t pointer_button;

	struct wl_list link;
} Seat;

static int sock_fd;
static char socketdir[256];
static char *socketpath;
static char sockbuf[4096];

static char *stdinbuf;
static size_t stdinbuf_cap;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;
static struct znet_tapesoftware_dwl_wm_v1 *dwl_wm;
static struct wl_cursor_image *cursor_image;
static struct wl_surface *cursor_surface;

static struct wl_list bar_list, seat_list;

static char **tags;
static uint32_t tags_l, tags_c;
static char **layouts;
static uint32_t layouts_l, layouts_c;

static struct fcft_font *font;
static uint32_t height, textpadding, buffer_scale;

static bool run_display;

#include "config.h"

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

/* Shared memory support function adapted from [wayland-book] */
static int
allocate_shm_file(size_t size)
{
	int fd = memfd_create("surface", MFD_CLOEXEC);
	if (fd == -1)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret == -1 && errno == EINTR);
	if (ret == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

static uint32_t
draw_text(char *text,
	  uint32_t x,
	  uint32_t y,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t *fg_color,
	  pixman_color_t *bg_color,
	  uint32_t max_x,
	  uint32_t buf_height,
	  uint32_t padding,
	  Color *colors,
	  uint32_t colors_l)
{
	if (!text || !*text || !max_x)
		return x;

	uint32_t ix = x, nx;

	if ((nx = x + padding) + padding >= max_x)
		return x;
	x = nx;

	bool draw_fg = foreground && fg_color;
	bool draw_bg = background && bg_color;
	
	pixman_image_t *fg_fill;
	pixman_color_t *cur_bg_color;
	if (draw_fg)
		fg_fill = pixman_image_create_solid_fill(fg_color);
	if (draw_bg)
		cur_bg_color = bg_color;

	uint32_t color_ind = 0, codepoint, state = UTF8_ACCEPT, last_cp = 0;
	for (char *p = text; *p; p++) {
		/* Check for new colors */
		if (state == UTF8_ACCEPT && colors && (draw_fg || draw_bg)) {
			while (color_ind < colors_l && p == colors[color_ind].start) {
				if (colors[color_ind].bg) {
					if (draw_bg)
						cur_bg_color = &colors[color_ind].color;
				} else if (draw_fg) {
					pixman_image_unref(fg_fill);
					fg_fill = pixman_image_create_solid_fill(&colors[color_ind].color);
				}
				color_ind++;
			}
		}
		
		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		/* Adjust x position based on kerning with previous glyph */
		long kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &kern, NULL);
		if ((nx = x + kern + glyph->advance.x) + padding > max_x)
			break;
		last_cp = codepoint;
		x += kern;

		if (draw_fg) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fg_fill, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fg_fill, glyph->pix, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			}
		}
		
		if (draw_bg) {
			pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
						cur_bg_color, 1, &(pixman_box32_t){
							.x1 = x, .x2 = nx,
							.y1 = 0, .y2 = buf_height
						});
		}
		
		/* increment pen position */
		x = nx;
	}
	
	if (draw_fg)
		pixman_image_unref(fg_fill);
	if (!last_cp)
		return ix;
	
	nx = x + padding;
	if (draw_bg) {
		/* Fill padding background */
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = ix, .x2 = ix + padding,
						.y1 = 0, .y2 = buf_height
					});
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = x, .x2 = nx,
						.y1 = 0, .y2 = buf_height
					});
	}
	
	return nx;
}

#define TEXT_WIDTH(text, maxwidth, padding)				\
	draw_text(text, 0, 0, NULL, NULL, NULL, NULL, maxwidth, 0, padding, NULL, 0)

static int
draw_frame(Bar *bar)
{
	/* Allocate buffer to be attached to the surface */
        int fd = allocate_shm_file(bar->bufsize);
	if (fd == -1)
		return -1;

	uint32_t *data = mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bar->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Pixman image corresponding to main buffer */
	pixman_image_t *final = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, data, bar->width * 4);
	
	/* Text background and foreground layers */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	
	/* Draw on images */
	uint32_t x = 0;
	uint32_t y = (bar->height + font->ascent - font->descent) / 2;
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	for (uint32_t i = 0; i < tags_l; i++) {
		const bool active = bar->mtags & 1 << i;
		const bool occupied = bar->ctags & 1 << i;
		const bool urgent = bar->urg & 1 << i;
		
		if (hide_vacant && !active && !occupied && !urgent)
			continue;

		pixman_color_t *fg_color = urgent ? &urgent_fg_color : (active ? &active_fg_color : &inactive_fg_color);
		pixman_color_t *bg_color = urgent ? &urgent_bg_color : (active ? &active_bg_color : &inactive_bg_color);
		
		if (!hide_vacant && occupied) {
			pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
						fg_color, 1, &(pixman_box32_t){
							.x1 = x + boxs, .x2 = x + boxs + boxw,
							.y1 = boxs, .y2 = boxs + boxw
						});
			if ((!bar->sel || !active) && boxw >= 3) {
				/* Make box hollow */
				pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
							&(pixman_color_t){ 0 },
							1, &(pixman_box32_t){
								.x1 = x + boxs + 1, .x2 = x + boxs + boxw - 1,
								.y1 = boxs + 1, .y2 = boxs + boxw - 1
							});
			}
		}
		
		x = draw_text(tags[i], x, y, foreground, background, fg_color, bg_color,
			      bar->width, bar->height, bar->textpadding, NULL, 0);
	}
	
	x = draw_text(bar->layout, x, y, foreground, background,
		      &inactive_fg_color, &inactive_bg_color, bar->width,
		      bar->height, bar->textpadding, NULL, 0);
	
	uint32_t status_width = TEXT_WIDTH(bar->status.text, bar->width - x, bar->textpadding);
	draw_text(bar->status.text, bar->width - status_width, y, foreground,
		  background, &inactive_fg_color, &inactive_bg_color,
		  bar->width, bar->height, bar->textpadding,
		  bar->status.colors, bar->status.colors_l);

	uint32_t nx;
	if (center_title) {
		uint32_t title_width = TEXT_WIDTH(custom_title ? bar->title.text : bar->window_title, bar->width - status_width - x, 0);
		nx = MAX(x, MIN((bar->width - title_width) / 2, bar->width - status_width - title_width));
	} else {
		nx = MIN(x + bar->textpadding, bar->width - status_width);
	}
	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
				bar->sel ? &active_bg_color : &inactive_bg_color, 1,
				&(pixman_box32_t){
					.x1 = x, .x2 = nx,
					.y1 = 0, .y2 = bar->height
				});
	x = nx;
	
	x = draw_text(custom_title ? bar->title.text : bar->window_title,
		      x, y, foreground, background,
		      bar->sel ? &active_fg_color : &inactive_fg_color,
		      bar->sel ? &active_bg_color : &inactive_bg_color,
		      bar->width - status_width, bar->height, 0,
		      custom_title ? bar->title.colors : NULL,
		      custom_title ? bar->title.colors_l : 0);

	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
				bar->sel ? &active_bg_color : &inactive_bg_color, 1,
				&(pixman_box32_t){
					.x1 = x, .x2 = bar->width - status_width,
					.y1 = 0, .y2 = bar->height
				});

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(final);
	
	munmap(data, bar->bufsize);

	wl_surface_set_buffer_scale(bar->wl_surface, buffer_scale);
	wl_surface_attach(bar->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
	wl_surface_commit(bar->wl_surface);

	return 0;
}

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			uint32_t serial, uint32_t w, uint32_t h)
{
	w = w * buffer_scale;
	h = h * buffer_scale;

	zwlr_layer_surface_v1_ack_configure(surface, serial);
	
	Bar *bar = (Bar *)data;
	
	if (bar->configured && w == bar->width && h == bar->height)
		return;
	
	bar->width = w;
	bar->height = h;
	bar->stride = bar->width * 4;
	bar->bufsize = bar->stride * bar->height;
	bar->configured = true;

	draw_frame(bar);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	run_display = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
cleanup(void)
{
	if (socketpath)
		unlink(socketpath);
}

static void
output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
	Bar *bar = (Bar *)data;
	
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!(bar->xdg_output_name = strdup(name)))
		EDIE("strdup");
}

static void
output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
			int32_t x, int32_t y)
{
}

static void
output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
		    int32_t width, int32_t height)
{
}

static void
output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
output_description(void *data, struct zxdg_output_v1 *xdg_output,
		   const char *description)
{
}

static const struct zxdg_output_v1_listener output_listener = {
	.name = output_name,
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = output_done,
	.description = output_description
};

static void
shell_command(char *command)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", command, NULL);
		exit(EXIT_SUCCESS);
	}
}

static void
pointer_enter(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface,
	      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	seat->bar = NULL;
	Bar *bar;
	wl_list_for_each(bar, &bar_list, link) {
		if (bar->wl_surface == surface) {
			seat->bar = bar;
			break;
		}
	}

	if (!cursor_image) {
		struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(NULL, 24 * buffer_scale, shm);
		cursor_image = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr")->images[0];
		cursor_surface = wl_compositor_create_surface(compositor);
        wl_surface_set_buffer_scale(cursor_surface, buffer_scale);
		wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0, 0);
		wl_surface_commit(cursor_surface);
	}
	wl_pointer_set_cursor(pointer, serial, cursor_surface,
			      cursor_image->hotspot_x,
			      cursor_image->hotspot_y);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface)
{
	Seat *seat = (Seat *)data;
	
	seat->bar = NULL;
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
	       uint32_t time, uint32_t button, uint32_t state)
{
	Seat *seat = (Seat *)data;

	seat->pointer_button = state == WL_POINTER_BUTTON_STATE_PRESSED ? button : 0;
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;
	
	seat->pointer_x = wl_fixed_to_int(surface_x);
	seat->pointer_y = wl_fixed_to_int(surface_y);
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	Seat *seat = (Seat *)data;

	if (!seat->pointer_button || !seat->bar)
		return;

	uint32_t x = 0, i = 0;
	do {
		if (hide_vacant) {
			const bool active = seat->bar->mtags & 1 << i;
			const bool occupied = seat->bar->ctags & 1 << i;
			const bool urgent = seat->bar->urg & 1 << i;
			if (!active && !occupied && !urgent)
				continue;
		}
		x += TEXT_WIDTH(tags[i], seat->bar->width - x, seat->bar->textpadding) / buffer_scale;
	} while (seat->pointer_x >= x && ++i < tags_l);
	
	if (i < tags_l) {
		/* Clicked on tags */
		if (ipc) {
			if (seat->pointer_button == BTN_LEFT)
				znet_tapesoftware_dwl_wm_monitor_v1_set_tags(seat->bar->dwl_wm_monitor, 1 << i, 1);
			else if (seat->pointer_button == BTN_MIDDLE)
				znet_tapesoftware_dwl_wm_monitor_v1_set_tags(seat->bar->dwl_wm_monitor, ~0, 1 << i);
			else if (seat->pointer_button == BTN_RIGHT)
				znet_tapesoftware_dwl_wm_monitor_v1_set_tags(seat->bar->dwl_wm_monitor, 0, 1 << i);
		}
	} else if (seat->pointer_x < (x += TEXT_WIDTH(seat->bar->layout, seat->bar->width - x, seat->bar->textpadding))) {
		/* Clicked on layout */
		if (ipc) {
			if (seat->pointer_button == BTN_LEFT)
				znet_tapesoftware_dwl_wm_monitor_v1_set_layout(seat->bar->dwl_wm_monitor, seat->bar->last_layout_idx);
			else if (seat->pointer_button == BTN_RIGHT)
				znet_tapesoftware_dwl_wm_monitor_v1_set_layout(seat->bar->dwl_wm_monitor, 2);
		}
	} else {
		uint32_t status_x = seat->bar->width / buffer_scale - TEXT_WIDTH(seat->bar->status.text, seat->bar->width - x, seat->bar->textpadding) / buffer_scale;
		if (seat->pointer_x < status_x) {
			/* Clicked on title */
			if (custom_title) {
				if (center_title) {
					uint32_t title_width = TEXT_WIDTH(seat->bar->title.text, status_x - x, 0);
					x = MAX(x, MIN((seat->bar->width - title_width) / 2, status_x - title_width));
				} else {
					x = MIN(x + seat->bar->textpadding, status_x);
				}
				for (i = 0; i < seat->bar->title.buttons_l; i++) {
					if (seat->pointer_button == seat->bar->title.buttons[i].btn
					    && seat->pointer_x >= x + seat->bar->title.buttons[i].x1
					    && seat->pointer_x < x + seat->bar->title.buttons[i].x2) {
						shell_command(seat->bar->title.buttons[i].command);
						break;
					}
				}
			}
		} else {
			/* Clicked on status */
			for (i = 0; i < seat->bar->status.buttons_l; i++) {
				if (seat->pointer_button == seat->bar->status.buttons[i].btn
				    && seat->pointer_x >= status_x + seat->bar->textpadding + seat->bar->status.buttons[i].x1 / buffer_scale
				    && seat->pointer_x < status_x + seat->bar->textpadding + seat->bar->status.buttons[i].x2 / buffer_scale) {
					shell_command(seat->bar->status.buttons[i].command);
					break;
				}
			}
		}
	}
	
	seat->pointer_button = 0;
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
	     uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer,
		    uint32_t axis_source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis)
{
}

static void
pointer_axis_value120(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.axis = pointer_axis,
	.axis_discrete = pointer_axis_discrete,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_value120 = pointer_axis_value120,
	.button = pointer_button,
	.enter = pointer_enter,
	.frame = pointer_frame,
	.leave = pointer_leave,
	.motion = pointer_motion,
};

static void
seat_capabilities(void *data, struct wl_seat *wl_seat,
		  uint32_t capabilities)
{
	Seat *seat = (Seat *)data;
	
	uint32_t has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && !seat->wl_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	} else if (!has_pointer && seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
dwl_wm_tag(void *data, struct znet_tapesoftware_dwl_wm_v1 *dwl_wm, const char *name)
{
	char **ptr;
	ARRAY_APPEND(tags, tags_l, tags_c, ptr);
	if (!(*ptr = strdup(name)))
		EDIE("strdup");
}

static void
dwl_wm_layout(void *data, struct znet_tapesoftware_dwl_wm_v1 *dwl_wm, const char *name)
{
	char **ptr;
	ARRAY_APPEND(layouts, layouts_l, layouts_c, ptr);
	if (!(*ptr = strdup(name)))
		EDIE("strdup");
}

static const struct znet_tapesoftware_dwl_wm_v1_listener dwl_wm_listener = {
	.tag = dwl_wm_tag,
	.layout = dwl_wm_layout
};

static void
dwl_wm_monitor_selected(void *data, struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor,
			uint32_t selected)
{
	Bar *bar = (Bar *)data;

	if (selected != bar->sel) {
		bar->sel = selected;
		bar->redraw = true;
	}
}

static void
dwl_wm_monitor_tag(void *data, struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor,
		   uint32_t tag, uint32_t state, uint32_t clients, int32_t focused_client)
{
	Bar *bar = (Bar *)data;

	uint32_t imtags = bar->mtags;
	uint32_t ictags = bar->ctags;
	uint32_t iurg = bar->urg;
	
	if (state & ZNET_TAPESOFTWARE_DWL_WM_MONITOR_V1_TAG_STATE_ACTIVE)
		bar->mtags |= 1 << tag;
	else
		bar->mtags &= ~(1 << tag);
	if (clients > 0)
		bar->ctags |= 1 << tag;
	else
		bar->ctags &= ~(1 << tag);
	if (state & ZNET_TAPESOFTWARE_DWL_WM_MONITOR_V1_TAG_STATE_URGENT)
		bar->urg |= 1 << tag;
	else
		bar->urg &= ~(1 << tag);

	if (bar->mtags != imtags || bar->ctags != ictags || bar->urg != iurg)
		bar->redraw = true;
}

static void
dwl_wm_monitor_layout(void *data, struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor,
		      uint32_t layout)
{
	Bar *bar = (Bar *)data;

	bar->layout = layouts[layout];
	bar->last_layout_idx = bar->layout_idx;
	bar->layout_idx = layout;
	bar->redraw = true;
}

static void
dwl_wm_monitor_title(void *data, struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor,
		     const char *title)
{
	if (custom_title)
		return;

	Bar *bar = (Bar *)data;

	if (bar->window_title)
		free(bar->window_title);
	if (!(bar->window_title = strdup(title)))
		EDIE("strdup");
	bar->redraw = true;
}

static void
dwl_wm_monitor_frame(void *data, struct znet_tapesoftware_dwl_wm_monitor_v1 *dwl_wm_monitor)
{
}

static const struct znet_tapesoftware_dwl_wm_monitor_v1_listener dwl_wm_monitor_listener = {
	.selected = dwl_wm_monitor_selected,
	.tag = dwl_wm_monitor_tag,
	.layout = dwl_wm_monitor_layout,
	.title = dwl_wm_monitor_title,
	.frame = dwl_wm_monitor_frame
};

static void
show_bar(Bar *bar)
{
	bar->wl_surface = wl_compositor_create_surface(compositor);
	if (!bar->wl_surface)
		DIE("Could not create wl_surface");

	bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar->wl_surface, bar->wl_output,
								   ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, PROGRAM);
	if (!bar->layer_surface)
		DIE("Could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

	zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, bar->height / buffer_scale);
	zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
					 (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->height / buffer_scale);
	wl_surface_commit(bar->wl_surface);

	bar->hidden = false;
}

static void
hide_bar(Bar *bar)
{
	zwlr_layer_surface_v1_destroy(bar->layer_surface);
	wl_surface_destroy(bar->wl_surface);

	bar->configured = false;
	bar->hidden = true;
}

static void
setup_bar(Bar *bar)
{
	bar->height = height * buffer_scale;
	bar->textpadding = textpadding;
	bar->bottom = bottom;
	bar->hidden = hidden;

	bar->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
	if (!bar->xdg_output)
		DIE("Could not create xdg_output");
	zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);

	if (ipc) {
		bar->dwl_wm_monitor = znet_tapesoftware_dwl_wm_v1_get_monitor(dwl_wm, bar->wl_output);
		if (!bar->dwl_wm_monitor)
			DIE("Could not create dwl_wm_monitor");
		znet_tapesoftware_dwl_wm_monitor_v1_add_listener(bar->dwl_wm_monitor, &dwl_wm_monitor_listener, bar);
	}
	
	if (!bar->hidden)
		show_bar(bar);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
	} else if (!strcmp(interface, znet_tapesoftware_dwl_wm_v1_interface.name)) {
		if (ipc) {
			dwl_wm = wl_registry_bind(registry, name, &znet_tapesoftware_dwl_wm_v1_interface, 1);
			znet_tapesoftware_dwl_wm_v1_add_listener(dwl_wm, &dwl_wm_listener, NULL);
		}
	} else if (!strcmp(interface, wl_output_interface.name)) {
		Bar *bar = calloc(1, sizeof(Bar));
		if (!bar)
			EDIE("calloc");
		bar->registry_name = name;
		bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		if (run_display)
			setup_bar(bar);
		wl_list_insert(&bar_list, &bar->link);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		Seat *seat = calloc(1, sizeof(Seat));
		if (!seat)
			EDIE("calloc");
		seat->registry_name = name;
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
		wl_list_insert(&seat_list, &seat->link);
	}
}

static void
teardown_bar(Bar *bar)
{
	if (bar->status.colors)
		free(bar->status.colors);
	if (bar->status.buttons)
		free(bar->status.buttons);
	if (bar->title.colors)
		free(bar->title.colors);
	if (bar->title.buttons)
		free(bar->title.buttons);
	if (!ipc && bar->layout)
		free(bar->layout);
	if (ipc)
		znet_tapesoftware_dwl_wm_monitor_v1_destroy(bar->dwl_wm_monitor);
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!bar->hidden) {
		zwlr_layer_surface_v1_destroy(bar->layer_surface);
		wl_surface_destroy(bar->wl_surface);
	}
	zxdg_output_v1_destroy(bar->xdg_output);
	wl_output_destroy(bar->wl_output);
	free(bar);
}

static void
teardown_seat(Seat *seat)
{
	if (seat->wl_pointer)
		wl_pointer_destroy(seat->wl_pointer);
	wl_seat_destroy(seat->wl_seat);
	free(seat);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	Bar *bar;
	Seat *seat;
	
	wl_list_for_each(bar, &bar_list, link) {
		if (bar->registry_name == name) {
			wl_list_remove(&bar->link);
			teardown_bar(bar);
			return;
		}
	}
	wl_list_for_each(seat, &seat_list, link) {
		if (seat->registry_name == name) {
			wl_list_remove(&seat->link);
			teardown_seat(seat);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove
};

static int
advance_word(char **beg, char **end)
{
	for (*beg = *end; **beg == ' '; (*beg)++);
	for (*end = *beg; **end && **end != ' '; (*end)++);
	if (!**end)
		/* last word */
		return -1;
	**end = '\0';
	(*end)++;
	return 0;
}

#define ADVANCE() advance_word(&wordbeg, &wordend)
#define ADVANCE_IF_LAST_CONT() if (ADVANCE() == -1) continue
#define ADVANCE_IF_LAST_RET() if (ADVANCE() == -1) return

static void
read_stdin(void)
{
	size_t len = 0;
	for (;;) {
		ssize_t rv = read(STDIN_FILENO, stdinbuf + len, stdinbuf_cap - len);
		if (rv == -1) {
			if (errno == EWOULDBLOCK)
				break;
			EDIE("read");
		}
		if (rv == 0) {
			run_display = false;
			return;
		}

		if ((len += rv) > stdinbuf_cap / 2)
			if (!(stdinbuf = realloc(stdinbuf, (stdinbuf_cap *= 2))))
				EDIE("realloc");
	}

	char *linebeg, *lineend;
	char *wordbeg, *wordend;
	
	for (linebeg = stdinbuf;
	     (lineend = memchr(linebeg, '\n', stdinbuf + len - linebeg));
	     linebeg = lineend) {
		*lineend++ = '\0';
		wordend = linebeg;

		ADVANCE_IF_LAST_CONT();

		Bar *it, *bar = NULL;
		wl_list_for_each(it, &bar_list, link) {
			if (it->xdg_output_name && !strcmp(wordbeg, it->xdg_output_name)) {
				bar = it;
				break;
			}
		}
		if (!bar)
			continue;
		
		ADVANCE_IF_LAST_CONT();

		uint32_t val;
		if (!strcmp(wordbeg, "tags")) {
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != bar->ctags) {
				bar->ctags = val;
				bar->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != bar->mtags) {
				bar->mtags = val;
				bar->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			/* skip sel */
			ADVANCE();
			if ((val = atoi(wordbeg)) != bar->urg) {
				bar->urg = val;
				bar->redraw = true;
			}
		} else if (!strcmp(wordbeg, "layout")) {
			if (bar->layout)
				free(bar->layout);
			if (!(bar->layout = strdup(wordend)))
				EDIE("strdup");
			bar->redraw = true;
		} else if (!strcmp(wordbeg, "title")) {
			if (custom_title)
				continue;
			if (bar->window_title)
				free(bar->window_title);
			if (!(bar->window_title = strdup(wordend)))
				EDIE("strdup");
			bar->redraw = true;
		} else if (!strcmp(wordbeg, "selmon")) {
			ADVANCE();
			if ((val = atoi(wordbeg)) != bar->sel) {
				bar->sel = val;
				bar->redraw = true;
			}
		}
	}
}

static void
set_top(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = false;
}

static void
set_bottom(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = true;
}

/* Color parsing logic adapted from [sway] */
static int
parse_color(const char *str, pixman_color_t *clr)
{
	if (*str == '#')
		str++;
	int len = strlen(str);

	// Disallows "0x" prefix that strtoul would ignore
	if ((len != 6 && len != 8) || !isxdigit(str[0]) || !isxdigit(str[1]))
		return -1;

	char *ptr;
	uint32_t parsed = strtoul(str, &ptr, 16);
	if (*ptr)
		return -1;

	if (len == 8) {
		clr->alpha = (parsed & 0xff) * 0x101;
		parsed >>= 8;
	} else {
		clr->alpha = 0xffff;
	}
	clr->red =   ((parsed >> 16) & 0xff) * 0x101;
	clr->green = ((parsed >>  8) & 0xff) * 0x101;
	clr->blue =  ((parsed >>  0) & 0xff) * 0x101;
	return 0;
}

static void
parse_into_customtext(CustomText *ct, char *text)
{
	ct->colors_l = ct->buttons_l = 0;

	if (status_commands) {
		uint32_t codepoint;
		uint32_t state = UTF8_ACCEPT;
		uint32_t last_cp = 0;
		uint32_t x = 0;
		size_t str_pos = 0;

		Button *left_button = NULL;
		Button *middle_button = NULL;
		Button *right_button = NULL;
	
		for (char *p = text; *p && str_pos < sizeof(ct->text) - 1; p++) {
			if (state == UTF8_ACCEPT && *p == '^') {
				p++;
				if (*p != '^') {
					char *arg, *end;
					if (!(arg = strchr(p, '(')) || !(end = strchr(arg + 1, ')')))
						continue;
					*arg++ = '\0';
					*end = '\0';
				
					if (!strcmp(p, "bg")) {
						Color *color;
						ARRAY_APPEND(ct->colors, ct->colors_l, ct->colors_c, color);
						if (!*arg)
							color->color = inactive_bg_color;
						else
							parse_color(arg, &color->color);
						color->bg = true;
						color->start = ct->text + str_pos;
					} else if (!strcmp(p, "fg")) {
						Color *color;
						ARRAY_APPEND(ct->colors, ct->colors_l, ct->colors_c, color);
						if (!*arg)
							color->color = inactive_fg_color;
						else
							parse_color(arg, &color->color);
						color->bg = false;
						color->start = ct->text + str_pos;
					} else if (!strcmp(p, "lm")) {
						if (left_button) {
							left_button->x2 = x;
							left_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, left_button);
							left_button->btn = BTN_LEFT;
							snprintf(left_button->command, sizeof left_button->command, "%s", arg);
							left_button->x1 = x;
						}
					} else if (!strcmp(p, "mm")) {
						if (middle_button) {
							middle_button->x2 = x;
							middle_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, middle_button);
							middle_button->btn = BTN_MIDDLE;
							snprintf(middle_button->command, sizeof middle_button->command, "%s", arg);
							middle_button->x1 = x;
						}
					} else if (!strcmp(p, "rm")) {
						if (right_button) {
							right_button->x2 = x;
							right_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, right_button);
							right_button->btn = BTN_RIGHT;
							snprintf(right_button->command, sizeof right_button->command, "%s", arg);
							right_button->x1 = x;
						}
					} 

					*--arg = '(';
					*end = ')';
				
					p = end;
					continue;
				}
			}

			ct->text[str_pos++] = *p;
			
			if (utf8decode(&state, &codepoint, *p))
				continue;
			
			const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
			if (!glyph)
				continue;
				
			long kern = 0;
			if (last_cp)
				fcft_kerning(font, last_cp, codepoint, &kern, NULL);
			last_cp = codepoint;
			
			x += kern + glyph->advance.x;
		}

		if (left_button)
			left_button->x2 = x;
		if (middle_button)
			middle_button->x2 = x;
		if (right_button)
			right_button->x2 = x;
		
		ct->text[str_pos] = '\0';
	} else {
		snprintf(ct->text, sizeof ct->text, "%s", text);
	}
}

static void
copy_customtext(CustomText *from, CustomText *to)
{
	snprintf(to->text, sizeof to->text, "%s", from->text);
	to->colors_l = to->buttons_l = 0;
	for (uint32_t i = 0; i < from->colors_l; i++) {
		Color *color;
		ARRAY_APPEND(to->colors, to->colors_l, to->colors_c, color);
		color->color = from->colors[i].color;
		color->bg = from->colors[i].bg;
		color->start = from->colors[i].start - (char *)&from->text + (char *)&to->text;
	}
	for (uint32_t i = 0; i < from->buttons_l; i++) {
		Button *button;
		ARRAY_APPEND(to->buttons, to->buttons_l, to->buttons_c, button);
		*button = from->buttons[i];
	}
}

static void
read_socket(void)
{
	int cli_fd;
	if ((cli_fd = accept(sock_fd, NULL, 0)) == -1)
		EDIE("accept");
	ssize_t len = recv(cli_fd, sockbuf, sizeof sockbuf - 1, 0);
	if (len == -1)
		EDIE("recv");
	close(cli_fd);
	if (len == 0)
		return;
	sockbuf[len] = '\0';

	char *wordbeg, *wordend;
	wordend = (char *)&sockbuf;

	ADVANCE_IF_LAST_RET();
		
	Bar *bar = NULL, *it;
	bool all = false;
		
	if (!strcmp(wordbeg, "all")) {
		all = true;
	} else if (!strcmp(wordbeg, "selected")) {
		wl_list_for_each(it, &bar_list, link) {
			if (it->sel) {
				bar = it;
				break;
			}
		}
	} else {
		wl_list_for_each(it, &bar_list, link) {
			if (it->xdg_output_name && !strcmp(wordbeg, it->xdg_output_name)) {
				bar = it;
				break;
			}
		}
	}
		
	if (!all && !bar)
		return;
	
	ADVANCE();

	if (!strcmp(wordbeg, "status")) {
		if (!*wordend)
			return;
		if (all) {
			Bar *first = NULL;
			wl_list_for_each(bar, &bar_list, link) {
				if (first) {
					copy_customtext(&first->status, &bar->status);
				} else {
					parse_into_customtext(&bar->status, wordend);
					first = bar;
				}
				bar->redraw = true;
			}
		} else {
			parse_into_customtext(&bar->status, wordend);
			bar->redraw = true;
		}
	} else if (!strcmp(wordbeg, "title")) {
		if (!custom_title || !*wordend)
			return;
		if (all) {
			Bar *first = NULL;
			wl_list_for_each(bar, &bar_list, link) {
				if (first) {
					copy_customtext(&first->title, &bar->title);
				} else {
					parse_into_customtext(&bar->title, wordend);
					first = bar;
				}
				bar->redraw = true;
			}
		} else {
			parse_into_customtext(&bar->title, wordend);
			bar->redraw = true;
		}
	} else if (!strcmp(wordbeg, "show")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->hidden)
					show_bar(bar);
		} else {
			if (bar->hidden)
				show_bar(bar);
		}
	} else if (!strcmp(wordbeg, "hide")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (!bar->hidden)
					hide_bar(bar);
		} else {
			if (!bar->hidden)
				hide_bar(bar);
		}
	} else if (!strcmp(wordbeg, "toggle-visibility")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->hidden)
					show_bar(bar);
				else
					hide_bar(bar);
		} else {
			if (bar->hidden)
				show_bar(bar);
			else
				hide_bar(bar);
		}
	} else if (!strcmp(wordbeg, "set-top")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->bottom)
					set_top(bar);
						
		} else {
			if (bar->bottom)
				set_top(bar);
		}
	} else if (!strcmp(wordbeg, "set-bottom")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (!bar->bottom)
					set_bottom(bar);
						
		} else {
			if (!bar->bottom)
				set_bottom(bar);
		}
	} else if (!strcmp(wordbeg, "toggle-location")) {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->bottom)
					set_top(bar);
				else
					set_bottom(bar);
		} else {
			if (bar->bottom)
				set_top(bar);
			else
				set_bottom(bar);
		}
	}
}

static void
event_loop(void)
{
	int wl_fd = wl_display_get_fd(display);

	while (run_display) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(wl_fd, &rfds);
		FD_SET(sock_fd, &rfds);
		if (!ipc)
			FD_SET(STDIN_FILENO, &rfds);

		wl_display_flush(display);

		if (select(MAX(sock_fd, wl_fd) + 1, &rfds, NULL, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			else
				EDIE("select");
		}
		
		if (FD_ISSET(wl_fd, &rfds))
			if (wl_display_dispatch(display) == -1)
				break;
		if (FD_ISSET(sock_fd, &rfds))
			read_socket();
		if (!ipc && FD_ISSET(STDIN_FILENO, &rfds))
			read_stdin();
		
		Bar *bar;
		wl_list_for_each(bar, &bar_list, link) {
			if (bar->redraw) {
				if (!bar->hidden)
					draw_frame(bar);
				bar->redraw = false;
			}
		}
	}
}

static void
client_send_command(struct sockaddr_un *sock_address, const char *output,
		    const char *cmd, const char *data)
{
	DIR *dir;
	if (!(dir = opendir(socketdir)))
		EDIE("Could not open directory '%s'", socketdir);

	if (data)
		snprintf(sockbuf, sizeof sockbuf, "%s %s %s", output, cmd, data);
	else
		snprintf(sockbuf, sizeof sockbuf, "%s %s", output, cmd);
	
	size_t len = strlen(sockbuf);
			
	struct dirent *de;
	bool newfd = true;

	/* Send data to all dwlb instances */
	while ((de = readdir(dir))) {
		if (!strncmp(de->d_name, "dwlb-", 5)) {
			if (newfd && (sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
				EDIE("socket");
			snprintf(sock_address->sun_path, sizeof sock_address->sun_path, "%s/%s", socketdir, de->d_name);
			if (connect(sock_fd, (struct sockaddr *) sock_address, sizeof(*sock_address)) == -1) {
				newfd = false;
				continue;
			}
			if (send(sock_fd, sockbuf, len, 0) == -1)
				fprintf(stderr, "Could not send status data to '%s'\n", sock_address->sun_path);
			close(sock_fd);
			newfd = true;
		}
	}
			
	closedir(dir);
}

void
sig_handler(int sig)
{
	if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM)
		run_display = false;
}

int
main(int argc, char **argv)
{
	char *xdgruntimedir;
	struct sockaddr_un sock_address;
	Bar *bar, *bar2;
	Seat *seat, *seat2;

	/* Establish socket directory */
	if (!(xdgruntimedir = getenv("XDG_RUNTIME_DIR")))
		DIE("Could not retrieve XDG_RUNTIME_DIR");
	snprintf(socketdir, sizeof socketdir, "%s/dwlb", xdgruntimedir);
	if (mkdir(socketdir, S_IRWXU) == -1)
		if (errno != EEXIST)
			EDIE("Could not create directory '%s'", socketdir);
	sock_address.sun_family = AF_UNIX;

	/* Parse options */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-status")) {
			if (++i + 1 >= argc)
				DIE("Option -status requires two arguments");
			client_send_command(&sock_address, argv[i], "status", argv[i + 1]);
			return 0;
		} else if (!strcmp(argv[i], "-title")) {
			if (++i + 1 >= argc)
				DIE("Option -title requires two arguments");
			client_send_command(&sock_address, argv[i], "title", argv[i + 1]);
			return 0;
		} else if (!strcmp(argv[i], "-show")) {
			if (++i >= argc)
				DIE("Option -show requires an argument");
			client_send_command(&sock_address, argv[i], "show", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-hide")) {
			if (++i >= argc)
				DIE("Option -hide requires an argument");
			client_send_command(&sock_address, argv[i], "hide", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-toggle-visibility")) {
			if (++i >= argc)
				DIE("Option -toggle requires an argument");
			client_send_command(&sock_address, argv[i], "toggle-visibility", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-set-top")) {
			if (++i >= argc)
				DIE("Option -set-top requires an argument");
			client_send_command(&sock_address, argv[i], "set-top", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-set-bottom")) {
			if (++i >= argc)
				DIE("Option -set-bottom requires an argument");
			client_send_command(&sock_address, argv[i], "set-bottom", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-toggle-location")) {
			if (++i >= argc)
				DIE("Option -toggle-location requires an argument");
			client_send_command(&sock_address, argv[i], "toggle-location", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-ipc")) {
			ipc = true;
		} else if (!strcmp(argv[i], "-no-ipc")) {
			ipc = false;
		} else if (!strcmp(argv[i], "-hide-vacant-tags")) {
			hide_vacant = true;
		} else if (!strcmp(argv[i], "-no-hide-vacant-tags")) {
			hide_vacant = false;
		} else if (!strcmp(argv[i], "-bottom")) {
			bottom = true;
		} else if (!strcmp(argv[i], "-no-bottom")) {
			bottom = false;
		} else if (!strcmp(argv[i], "-hidden")) {
			hidden = true;
		} else if (!strcmp(argv[i], "-no-hidden")) {
			hidden = false;
		} else if (!strcmp(argv[i], "-status-commands")) {
			status_commands = true;
		} else if (!strcmp(argv[i], "-no-status-commands")) {
			status_commands = false;
		} else if (!strcmp(argv[i], "-center-title")) {
			center_title = true;
		} else if (!strcmp(argv[i], "-no-center-title")) {
			center_title = false;
		} else if (!strcmp(argv[i], "-custom-title")) {
			custom_title = true;
		} else if (!strcmp(argv[i], "-no-custom-title")) {
			custom_title = false;
		} else if (!strcmp(argv[i], "-font")) {
			if (++i >= argc)
				DIE("Option -font requires an argument");
			fontstr = argv[i];
		} else if (!strcmp(argv[i], "-vertical-padding")) {
			if (++i >= argc)
				DIE("Option -vertical-padding requires an argument");
			vertical_padding = MAX(MIN(atoi(argv[i]), 100), 0);
		} else if (!strcmp(argv[i], "-active-fg-color")) {
			if (++i >= argc)
				DIE("Option -active-fg-color requires an argument");
			if (parse_color(argv[i], &active_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-active-bg-color")) {
			if (++i >= argc)
				DIE("Option -active-bg-color requires an argument");
			if (parse_color(argv[i], &active_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-inactive-fg-color")) {
			if (++i >= argc)
				DIE("Option -inactive-fg-color requires an argument");
			if (parse_color(argv[i], &inactive_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-inactive-bg-color")) {
			if (++i >= argc)
				DIE("Option -inactive-bg-color requires an argument");
			if (parse_color(argv[i], &inactive_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urgent-fg-color")) {
			if (++i >= argc)
				DIE("Option -urgent-fg-color requires an argument");
			if (parse_color(argv[i], &urgent_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urgent-bg-color")) {
			if (++i >= argc)
				DIE("Option -urgent-bg-color requires an argument");
			if (parse_color(argv[i], &urgent_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-tags")) {
			if (++i + 1 >= argc)
				DIE("Option -tags requires at least two arguments");
			int v;
			if ((v = atoi(argv[i])) <= 0 || i + v >= argc)
				DIE("-tags: invalid arguments");
			if (tags) {
				for (uint32_t j = 0; j < tags_l; j++)
					free(tags[j]);
				free(tags);
			}
			if (!(tags = malloc(v * sizeof(char *))))
				EDIE("malloc");
			for (int j = 0; j < v; j++)
				if (!(tags[j] = strdup(argv[i + 1 + j])))
					EDIE("strdup");
			tags_l = tags_c = v;
			i += v;
		} else if (!strcmp(argv[i], "-scale")) {
			if (++i >= argc)
				DIE("Option -scale requires an argument");
			buffer_scale = strtoul(argv[i], &argv[i] + strlen(argv[i]), 10);
		} else if (!strcmp(argv[i], "-v")) {
			fprintf(stderr, PROGRAM " " VERSION "\n");
			return 0;
		} else if (!strcmp(argv[i], "-h")) {
			fprintf(stderr, USAGE);
			return 0;
		} else {
			DIE("Option '%s' not recognized\n" USAGE, argv[i]);
		}
	}

	/* Set up display and protocols */
	display = wl_display_connect(NULL);
	if (!display)
		DIE("Failed to create display");

	wl_list_init(&bar_list);
	wl_list_init(&seat_list);
	
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell || !output_manager || (ipc && !dwl_wm))
		DIE("Compositor does not support all needed protocols");

	/* Load selected font */
	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);

	unsigned int dpi = 96 * buffer_scale;
	char buf[10];
	snprintf(buf, sizeof buf, "dpi=%u", dpi);
	if (!(font = fcft_from_name(1, (const char *[]) {fontstr}, buf)))
		DIE("Could not load font");
	textpadding = font->height / 2;
	height = font->height / buffer_scale + vertical_padding * 2;

	/* Configure tag names */
	if (ipc && tags) {
		for (uint32_t i = 0; i < tags_l; i++)
			free(tags[i]);
		free(tags);
		tags_l = tags_c = 0;
		tags = NULL;
	} else if (!ipc && !tags) {
		if (!(tags = malloc(LENGTH(tags_noipc) * sizeof(char *))))
			EDIE("malloc");
		tags_l = tags_c = LENGTH(tags_noipc);
		for (uint32_t i = 0; i < tags_l; i++)
			if (!(tags[i] = strdup(tags_noipc[i])))
				EDIE("strdup");
	}
	
	/* Setup bars */
	wl_list_for_each(bar, &bar_list, link)
		setup_bar(bar);
	wl_display_roundtrip(display);

	if (!ipc) {
		/* Configure stdin */
		if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
			EDIE("fcntl");
	
		/* Allocate stdin buffer */
		if (!(stdinbuf = malloc(1024)))
			EDIE("malloc");
		stdinbuf_cap = 1024;
	}

	/* Set up socket */
	bool found = false;
	for (uint32_t i = 0; i < 50; i++) {
		if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
			DIE("socket");
		snprintf(sock_address.sun_path, sizeof sock_address.sun_path, "%s/dwlb-%i", socketdir, i);
		if (connect(sock_fd, (struct sockaddr *)&sock_address, sizeof sock_address) == -1) {
			found = true;
			break;
		}
		close(sock_fd);
	}
	if (!found)
		DIE("Could not secure a socket path");

	socketpath = (char *)&sock_address.sun_path;
	unlink(socketpath);
	if (bind(sock_fd, (struct sockaddr *)&sock_address, sizeof sock_address) == -1)
		EDIE("bind");
	if (listen(sock_fd, SOMAXCONN) == -1)
		EDIE("listen");
	fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(sock_fd, F_GETFD));

	/* Set up signals */
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGCHLD, SIG_IGN);
	
	/* Run */
	run_display = true;
	event_loop();

	/* Clean everything up */
	close(sock_fd);
	unlink(socketpath);
	
	if (!ipc)
		free(stdinbuf);

	if (tags) {
		for (uint32_t i = 0; i < tags_l; i++)
			free(tags[i]);
		free(tags);
	}
	if (layouts) {
		for (uint32_t i = 0; i < layouts_l; i++)
			free(layouts[i]);
		free(layouts);
	}

	wl_list_for_each_safe(bar, bar2, &bar_list, link)
		teardown_bar(bar);
	wl_list_for_each_safe(seat, seat2, &seat_list, link)
		teardown_seat(seat);
	
	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	if (ipc)
		znet_tapesoftware_dwl_wm_v1_destroy(dwl_wm);
	
	fcft_destroy(font);
	fcft_fini();
	
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
