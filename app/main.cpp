#define GST_USE_UNSTABLE_API
#define HAVE_MEMFD_CREATE

#include <string>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <signal.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include "utils.h"
#include "xdg-shell-client-protocol.h"
#include "AglShellGrpcClient.h"

#include <gst/gst.h>

#include <gst/video/videooverlay.h>
#include <gst/wayland/wayland.h>

#if !GST_CHECK_VERSION(1, 22, 0)
#define gst_is_wl_display_handle_need_context_message gst_is_wayland_display_handle_need_context_message
#define gst_wl_display_handle_context_new gst_wayland_display_handle_context_new
#endif

#ifndef APP_DATA_PATH
#define APP_DATA_PATH /usr/share/applications/data
#endif

// these only applies if the window is a dialog/pop-up one
// by default the compositor make the window maximized
#define WINDOW_WIDTH_SIZE	640
#define WINDOW_HEIGHT_SIZE	720

#define WINDOW_WIDTH_POS_X	640
#define WINDOW_WIDTH_POS_Y	180

#define MAX_BUFFER_ALLOC	2

// C++ requires a cast and we in wayland we do the cast implictly
#define WL_ARRAY_FOR_EACH(pos, array, type) \
	for (pos = (type)(array)->data; \
	     (const char *) pos < ((const char *) (array)->data + (array)->size); \
	     (pos)++)

struct display {
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_compositor *wl_compositor;
	struct wl_output *wl_output;
	struct wl_shm *shm;

	struct {
		int width;
		int height;
	} output_data;

	struct xdg_wm_base *wm_base;
	int has_xrgb;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;
	int busy;
	int width, height;
	size_t size;    /* width * 4 * height */
	struct wl_list buffer_link; /** window::buffer_list */
};

struct window {
	struct display *display;

	int x, y;
	int width, height;
	int init_width;
	int init_height;

	struct wl_surface *surface;

	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	bool wait_for_configure;

	int fullscreen, maximized;

	struct wl_list buffer_list;
	struct wl_callback *callback;
	bool needs_update_buffer;
};


struct receiver_data {
	struct window *window;

	GstElement *pipeline;
	GstVideoOverlay *overlay;
};

static int running = 1;
static bool gst_pipeline_failed = FALSE;
static bool fallback_gst_pipeline_tried = FALSE;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time);

static struct buffer *
alloc_buffer(struct window *window, int width, int height)
{
	struct buffer *buffer = static_cast<struct buffer *>(calloc(1, sizeof(*buffer)));

	buffer->width = width;
	buffer->height = height;
	wl_list_insert(&window->buffer_list, &buffer->buffer_link);

	return buffer;
}

static void
destroy_buffer(struct buffer *buffer)
{
	if (buffer->buffer)
		wl_buffer_destroy(buffer->buffer);

	munmap(buffer->shm_data, buffer->size);
	wl_list_remove(&buffer->buffer_link);
	free(buffer);
}

static struct buffer *
pick_free_buffer(struct window *window)
{
	struct buffer *b;
	struct buffer *buffer = NULL;

	wl_list_for_each(b, &window->buffer_list, buffer_link) {
		if (!b->busy) {
			buffer = b;
			break;
		}
	}

	return buffer;
}

static void
prune_old_released_buffers(struct window *window)
{
	struct buffer *b, *b_next;

	wl_list_for_each_safe(b, b_next,
			      &window->buffer_list, buffer_link) {
		if (!b->busy && (b->width != window->width ||
				 b->height != window->height))
			destroy_buffer(b);
	}
}

static void
paint_pixels(void *image, int padding, int width, int height, uint32_t time)
{
	memset(image, 0x00, width * height * 4);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = static_cast<struct buffer *>(data);
	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};


static int
create_shm_buffer(struct display *display, struct buffer *buffer,
		  int width, int height, uint32_t format)
{
	struct wl_shm_pool *pool;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %s\n",
				size, strerror(errno));
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(display->shm, fd, size);
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width,
						   height, stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	buffer->shm_data = data;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;

	fprintf(stdout, "Created shm buffer with width %d, height %d\n", width, height);

	return 0;
}

static struct buffer *
get_next_buffer(struct window *window)
{
	struct buffer *buffer = NULL;
	int ret = 0;

	if (window->needs_update_buffer) {
		int i;

		for (i = 0; i < MAX_BUFFER_ALLOC; i++)
			alloc_buffer(window, window->width, window->height);

		window->needs_update_buffer = false;
	}

	buffer = pick_free_buffer(window);
	if (!buffer)
		return NULL;


	if (!buffer->buffer) {
		ret = create_shm_buffer(window->display, buffer, window->width,
					window->height, WL_SHM_FORMAT_XRGB8888);

		if (ret < 0)
			return NULL;

		/* paint the padding */
		memset(buffer->shm_data, 0x00, window->width * window->height * 4);
	}

	return buffer;
}


static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = static_cast<struct window *>(data);
	struct buffer *buffer;

	prune_old_released_buffers(window);

	buffer = get_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
				!callback ? "Failed to create the first buffer.\n" :
				"Both buffers busy at redraw(). Server bug?\n");
		abort();
	}

	// do the actual painting
	paint_pixels(buffer->shm_data, 0x0, window->width, window->height, time);

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);

	buffer->busy = 1;
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = static_cast<struct display *>(data);

	if (format == WL_SHM_FORMAT_XRGB8888)
		d->has_xrgb = true;
}

static const struct wl_shm_listener shm_listener = {
	shm_format
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
        xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
        xdg_wm_base_ping,
};

static void
display_handle_geometry(void *data, struct wl_output *wl_output,
			int x, int y, int physical_width, int physical_height,
			int subpixel, const char *make, const char *model, int transform)
{
	(void) data;
	(void) wl_output;
	(void) x;
	(void) y;
	(void) physical_width;
	(void) physical_height;
	(void) subpixel;
	(void) make;
	(void) model;
	(void) transform;
}

static void
display_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		    int width, int height, int refresh)
{
	struct display *d = static_cast<struct display *>(data);

	if (wl_output == d->wl_output && (flags & WL_OUTPUT_MODE_CURRENT)) {
		d->output_data.width = width;
		d->output_data.height = height;

		fprintf(stdout, "Found output with width %d and height %d\n",
				d->output_data.width, d->output_data.height);
	}
}

static void
display_handle_scale(void *data, struct wl_output *wl_output, int scale)
{
	(void) data;
	(void) wl_output;
	(void) scale;
}

static void
display_handle_done(void *data, struct wl_output *wl_output)
{
	(void) data;
	(void) wl_output;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode,
	display_handle_done,
	display_handle_scale
};


static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t id,
		       const char *interface, uint32_t version)
{
	struct display *d = static_cast<struct display *>(data);

	if (strcmp(interface, "wl_compositor") == 0) {
		d->wl_compositor =
			static_cast<struct wl_compositor *>(wl_registry_bind(registry, id,
						    &wl_compositor_interface, 1));
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->wm_base = static_cast<struct xdg_wm_base *>(wl_registry_bind(registry,
				id, &xdg_wm_base_interface, 1));
		xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = static_cast<struct wl_shm *>(wl_registry_bind(registry,
				id, &wl_shm_interface, 1));
		wl_shm_add_listener(d->shm, &shm_listener, d);
	} else if (strcmp(interface, "wl_output") == 0) {
		d->wl_output = static_cast<struct wl_output *>(wl_registry_bind(registry, id,
					     &wl_output_interface, 1));
		wl_output_add_listener(d->wl_output, &output_listener, d);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *reg, uint32_t id)
{
	(void) data;
	(void) reg;
	(void) id;
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove,
};


static void
error_cb(GstBus *bus, GstMessage *msg, gpointer user_data)
{
	struct receiver_data *d =
		static_cast<struct receiver_data *>(user_data);

	gchar *debug = NULL;
	GError *err = NULL;

	gst_message_parse_error(msg, &err, &debug);

	g_print("Error: %s\n", err->message);
	g_error_free(err);

	if (debug) {
		g_print("Debug details: %s\n", debug);
		g_free(debug);
	}

	gst_element_set_state(d->pipeline, GST_STATE_NULL);
}

static GstBusSyncReply
bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data)
{
	struct receiver_data *d =
		static_cast<struct receiver_data *>(user_data);

	if (gst_is_wl_display_handle_need_context_message(message)) {
		GstContext *context;
		struct wl_display *display_handle = d->window->display->wl_display;

		context = gst_wl_display_handle_context_new(display_handle);
		gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);

		goto drop;
	} else if (gst_is_video_overlay_prepare_window_handle_message(message)) {
		struct wl_surface *window_handle = d->window->surface;

		/* GST_MESSAGE_SRC(message) will be the overlay object that we
		 * have to use. This may be waylandsink, but it may also be
		 * playbin. In the latter case, we must make sure to use
		 * playbin instead of waylandsink, because playbin resets the
		 * window handle and render_rectangle after restarting playback
		 * and the actual window size is lost */
		d->overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message));

		g_print("setting window handle and size (%d x %d) w %d, h %d\n",
				d->window->x, d->window->y,
				d->window->width, d->window->height);

		gst_video_overlay_set_window_handle(d->overlay, (guintptr) window_handle);
		gst_video_overlay_set_render_rectangle(d->overlay,
						       d->window->x, d->window->y,
						       d->window->width, d->window->height);

		goto drop;
	}
	else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
		GError* err = NULL;
		gchar* dbg_info = NULL;

		gst_message_parse_error(message, &err, &dbg_info);
		g_printerr("ERROR from element %s: %s code %d\n",
			GST_OBJECT_NAME(message->src), err->message, err->code);
		g_printerr("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
		gst_pipeline_failed = TRUE;
		g_error_free(err);
		g_free(dbg_info);
		goto drop;
	}

	return GST_BUS_PASS;

drop:
	gst_message_unref(message);
	return GST_BUS_DROP;
}

static void
handle_xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	struct window *window = static_cast<struct window *>(data);

	xdg_surface_ack_configure(surface, serial);

	if (window->wait_for_configure) {
		redraw(window, NULL, 0);
		window->wait_for_configure = false;
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	handle_xdg_surface_configure,
};

static void
handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
	struct window *window = static_cast<struct window *>(data);
	uint32_t *p;

	window->fullscreen = 0;
	window->maximized = 0;

	// use our own macro as C++ can't typecast from (void *) directly
	WL_ARRAY_FOR_EACH(p, states, uint32_t *) {
		uint32_t state = *p;
		switch (state) {
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
			window->fullscreen = 1;
			break;
		case XDG_TOPLEVEL_STATE_MAXIMIZED:
			window->maximized = 1;
			break;
		}
	}

	if (width > 0 && height > 0) {
		if (!window->fullscreen && !window->maximized) {
			window->init_width = width;
			window->init_height = height;
		}
		window->width = width;
		window->height = height;
	} else if (!window->fullscreen && !window->maximized) {
		window->width = window->init_width;
		window->height = window->init_height;
	}

	window->needs_update_buffer = true;

}

static void
handle_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	handle_xdg_toplevel_configure,
	handle_xdg_toplevel_close,
};

static struct window *
create_window(struct display *display, int width, int height, const char *app_id)
{
	struct window *window;
	int i;

	assert(display->wm_base != NULL);

	window = static_cast<struct window *>(calloc(1, sizeof(*window)));
	if (!window)
		return NULL;

	wl_list_init(&window->buffer_list);
	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->init_width = width;
	window->init_height = height;
	window->surface = wl_compositor_create_surface(display->wl_compositor);

	if (display->wm_base) {
		window->xdg_surface =
			xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
		assert(window->xdg_surface);

		xdg_surface_add_listener(window->xdg_surface,
					 &xdg_surface_listener, window);
		window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
		assert(window->xdg_toplevel);

		xdg_toplevel_add_listener(window->xdg_toplevel,
					  &xdg_toplevel_listener, window);

		xdg_toplevel_set_app_id(window->xdg_toplevel, app_id);

		wl_surface_commit(window->surface);
		window->wait_for_configure = true;
	}

	for (i = 0; i < MAX_BUFFER_ALLOC; i++)
		alloc_buffer(window, window->width, window->height);

	return window;
}


static void
destroy_window(struct window *window)
{
	struct buffer *buffer, *buffer_next;

	if (window->callback)
		wl_callback_destroy(window->callback);

	wl_list_for_each_safe(buffer, buffer_next,
			      &window->buffer_list, buffer_link)
		destroy_buffer(buffer);

	if (window->xdg_toplevel)
		xdg_toplevel_destroy(window->xdg_toplevel);

	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);

	wl_surface_destroy(window->surface);
	free(window);
}

static void
signal_int(int sig, siginfo_t *si, void *_unused)
{
	running = 0;
}

static struct display *
create_display(int argc, char *argv[])
{
	struct display *display;

	display = static_cast<struct display *>(calloc(1, sizeof(*display)));
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	display->wl_display = wl_display_connect(NULL);
	assert(display->wl_display);

	display->has_xrgb = false;
	display->wl_registry = wl_display_get_registry(display->wl_display);

	wl_registry_add_listener(display->wl_registry, &registry_listener, display);
	wl_display_roundtrip(display->wl_display);

	if (display->shm == NULL) {
		fprintf(stderr, "No wl_shm global\n");
		return NULL;
	}

	wl_display_roundtrip(display->wl_display);

	if (!display->has_xrgb) {
		fprintf(stderr, "WL_SHM_FORMAT_XRGB32 not available\n");
		return NULL;
	}

	return display;
}

static void
destroy_display(struct display *display)
{
	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->wm_base)
		xdg_wm_base_destroy(display->wm_base);

	if (display->wl_compositor)
		wl_compositor_destroy(display->wl_compositor);

	wl_registry_destroy(display->wl_registry);
	wl_display_flush(display->wl_display);
	wl_display_disconnect(display->wl_display);
	free(display);
}

// stringify the un-quoted string to quoted C string
#define xstr(a) str(a)
#define str(a) #a

GstElement* create_pipeline(int* argc, char** argv[])
{
	GError *error = NULL;
	const char *camera_device = NULL;
	const char *width_str = NULL;
	const char *height_str = NULL;
	int width;
	int height;

	// pipewire is default.
	char *v4l2_path = getenv("ENABLE_V4L2_PATH");
	bool v4l2 = false;

	char pipeline_str[1024];

	camera_device = getenv("DEFAULT_V4L2_DEVICE");
	if (!camera_device)
		camera_device = get_first_camera_device();

	width_str = getenv("DEFAULT_DEVICE_WIDTH");
	if (!width_str)
		width = WINDOW_WIDTH_SIZE;
	else
		width = atoi(width_str);

	height_str = getenv("DEFAULT_DEVICE_HEIGHT");
	if (!height_str)
		height = WINDOW_HEIGHT_SIZE;
	else
		height = atoi(height_str);

	if (v4l2_path == NULL)
		v4l2 = false;
	else if (g_str_equal(v4l2_path, "yes") || g_str_equal(v4l2_path, "true"))
		v4l2 = true;

	memset(pipeline_str, 0, sizeof(pipeline_str));

	if (v4l2)
		snprintf(pipeline_str, sizeof(pipeline_str), "v4l2src device=%s ! video/x-raw,width=%d,height=%d ! waylandsink",
			camera_device, width, height);
	else if (gst_pipeline_failed == TRUE) {
		snprintf(pipeline_str, sizeof(pipeline_str), "filesrc location=%s/still-image.jpg ! decodebin ! videoconvert ! imagefreeze ! waylandsink",
			xstr(APP_DATA_PATH));
		fallback_gst_pipeline_tried = TRUE;
	}
	else {
		snprintf(pipeline_str, sizeof(pipeline_str), "pipewiresrc ! waylandsink");
	}

	fprintf(stdout, "Using pipeline: %s\n", pipeline_str);

	GstElement *pipeline = gst_parse_launch(pipeline_str, &error);

	if (error || !pipeline) {
		fprintf(stderr, "gstreamer pipeline construction failed!\n");
		free(argv);
		return NULL;
	}

	return pipeline;
}

int main(int argc, char* argv[])
{
	int ret = 0;
	struct sigaction sa;
	struct receiver_data receiver_data = {};
	struct display* display;
	struct window* window;
	const char* app_id = "camera-gstreamer";

	// for starting the application from the beginning, with a diffrent
	// role we need to handle that creating the main window
	if (argc >= 2 && strcmp(argv[1], "float") == 0) {
		GrpcClient *client = new GrpcClient();
		client->SetAppFloat(std::string(app_id), 30, 400);
	}

	sa.sa_sigaction = signal_int;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND | SA_SIGINFO;
	sigaction(SIGINT, &sa, NULL);

	int gargc = 2;
	char** gargv = static_cast<char**>(calloc(2, sizeof(char*)));

	gargv[0] = strdup(argv[0]);
	gargv[1] = strdup("--gst-debug-level=2");

	setbuf(stdout, NULL);

	gst_init(&gargc, &gargv);

	receiver_data.pipeline = create_pipeline(&gargc, &gargv);

	if (!receiver_data.pipeline)
		return EXIT_FAILURE;

	display = create_display(argc, argv);
	if (!display)
		return -1;

	// we use the role to set a correspondence between the top level
	// surface and our application, with the previous call letting the
	// compositor know that we're one and the same
	window = create_window(display, WINDOW_WIDTH_SIZE, WINDOW_HEIGHT_SIZE, app_id);

	if (!window) {
		free(gargv);
		return EXIT_FAILURE;
	}

	window->display = display;
	receiver_data.window = window;

	/* Initialise damage to full surface, so the padding gets painted */
	wl_surface_damage(window->surface, 0, 0,
			  window->width, window->height);

	if (!window->wait_for_configure) {
		redraw(window, NULL, 0);
	}

	GstBus *bus = gst_element_get_bus(receiver_data.pipeline);
	gst_bus_add_signal_watch(bus);

	g_signal_connect(bus, "message::error", G_CALLBACK(error_cb), &receiver_data);
	gst_bus_set_sync_handler(bus, bus_sync_handler, &receiver_data, NULL);
	gst_object_unref(bus);

	gst_element_set_state(receiver_data.pipeline, GST_STATE_PLAYING);
	fprintf(stdout, "gstreamer pipeline running\n");

	// run the application
	while (running && ret != -1) {
		ret = wl_display_dispatch(display->wl_display);
		if (gst_pipeline_failed && fallback_gst_pipeline_tried == FALSE) {
			gst_element_set_state(receiver_data.pipeline, GST_STATE_NULL);
			gst_object_unref(receiver_data.pipeline);
			/* retry with fallback pipeline */
			receiver_data.pipeline = create_pipeline(&gargc, &gargv);
			GstBus *bus = gst_element_get_bus(receiver_data.pipeline);
			gst_bus_add_signal_watch(bus);
			g_signal_connect(bus, "message::error", G_CALLBACK(error_cb), &receiver_data);
			gst_bus_set_sync_handler(bus, bus_sync_handler, &receiver_data, NULL);
			gst_object_unref(bus);
			gst_element_set_state(receiver_data.pipeline, GST_STATE_PLAYING);
		}
	}
	gst_element_set_state(receiver_data.pipeline, GST_STATE_NULL);
	gst_object_unref(receiver_data.pipeline);

	destroy_window(window);
	destroy_display(display);
	free(gargv);

	return ret;
}
