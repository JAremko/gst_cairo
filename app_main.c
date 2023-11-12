#include <gst/gst.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <sys/stat.h>
#include <cairo.h>
#include <cairo-gobject.h>
#include <glib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#define OVERLAY_FILE "/tmp/overlay"
#define BUFFER_WIDTH 800
#define BUFFER_HEIGHT 600
static unsigned char buffer[BUFFER_WIDTH * BUFFER_HEIGHT * 4] = {0};


static gboolean on_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    GMainLoop *loop = (GMainLoop *)user_data;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug;

            gst_message_parse_error(message, &err, &debug);
            g_critical("Got ERROR: %s (%s)", err->message, GST_STR_NULL(debug));
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *debug;

            gst_message_parse_warning(message, &err, &debug);
            g_warning("Got WARNING: %s (%s)", err->message, GST_STR_NULL(debug));
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }

    return TRUE;
}

typedef struct {
    int fd;
    unsigned char *data;
    size_t size;
    gboolean is_mapped;
} MappedOverlayFile;

typedef struct {
    gboolean valid;
    GstVideoInfo vinfo;
    cairo_surface_t *overlay_surface;
    MappedOverlayFile mapped_overlay;
    cairo_surface_t *surface;

} CairoOverlayState;


static void map_overlay_file(CairoOverlayState *state, const char *filename);
static void unmap_overlay_file(CairoOverlayState *state);
static void copy_overlay_data(CairoOverlayState *state);

static void prepare_overlay(GstElement *overlay, GstCaps *caps, gpointer user_data) {
    CairoOverlayState *state = (CairoOverlayState *)user_data;
    state->valid = gst_video_info_from_caps(&state->vinfo, caps);
}

static void draw_overlay(GstElement *overlay, cairo_t *cr, guint64 timestamp, guint64 duration, gpointer user_data) {
    CairoOverlayState *s = (CairoOverlayState *)user_data;

    if (!s->valid || s->surface == NULL)
        return;

    copy_overlay_data(s);

    // Update the existing surface's data
    unsigned char *surface_data = cairo_image_surface_get_data(s->surface);
    memcpy(surface_data, buffer, BUFFER_WIDTH * BUFFER_HEIGHT * 4);
    cairo_surface_mark_dirty(s->surface); // Tell Cairo that the surface has been modified

    cairo_set_source_surface(cr, s->surface, 0, 0);
    cairo_paint(cr);
}

static GstElement *setup_gst_pipeline(CairoOverlayState *overlay_state) {
    GstElement *pipeline;
    GstElement *cairo_overlay;
    GstElement *source, *adaptor1, *adaptor2, *sink, *videoscale;
    GstCaps *caps;

    pipeline = gst_pipeline_new("cairo-overlay-example");

    source = gst_element_factory_make("videotestsrc", "source");
    adaptor1 = gst_element_factory_make("videoconvert", "adaptor1");
    cairo_overlay = gst_element_factory_make("cairooverlay", "overlay");
    videoscale = gst_element_factory_make("videoscale", "videoscale");
    adaptor2 = gst_element_factory_make("videoconvert", "adaptor2");
    sink = gst_element_factory_make("ximagesink", "sink");

    g_assert(cairo_overlay);
    g_assert(videoscale);

    // Initialize the reusable surface
    overlay_state->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BUFFER_WIDTH, BUFFER_HEIGHT);

    // Set the desired output size
    caps = gst_caps_new_simple("video/x-raw",
                               "width", G_TYPE_INT, BUFFER_WIDTH,
                               "height", G_TYPE_INT, BUFFER_HEIGHT,
                               NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, adaptor1, cairo_overlay, videoscale, adaptor2, sink, NULL);

    if (!gst_element_link_many(source, adaptor1, cairo_overlay, adaptor2, videoscale, NULL)) {
        g_warning("Failed to link elements up to videoscale!");
    }

    if (!gst_element_link_filtered(videoscale, sink, caps)) {
        g_warning("Failed to link videoscale to sink!");
    }

    gst_caps_unref(caps);

    // Connect signals for cairooverlay
    g_signal_connect(cairo_overlay, "draw", G_CALLBACK(draw_overlay), overlay_state);
    g_signal_connect(cairo_overlay, "caps-changed", G_CALLBACK(prepare_overlay), overlay_state);

    return pipeline;
}

static void map_overlay_file(CairoOverlayState *state, const char *filename) {
    struct stat sb;
    int retries = 0;
    const useconds_t retry_delay = 100000; // Retry every 100 milliseconds

    state->mapped_overlay.fd = open(filename, O_RDONLY);
    if (state->mapped_overlay.fd == -1) {
        perror("Error opening file for reading");
        exit(1);
    }

    while (fstat(state->mapped_overlay.fd, &sb) == -1 || sb.st_size == 0) {
        usleep(retry_delay); // Wait for half a second before retrying
    }

    state->mapped_overlay.size = sb.st_size;
    state->mapped_overlay.data = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, state->mapped_overlay.fd, 0);
    if (state->mapped_overlay.data == MAP_FAILED) {
        perror("Error mmapping the file");
        close(state->mapped_overlay.fd);
        exit(1);
    }

    msync(state->mapped_overlay.data, state->mapped_overlay.size, MS_SYNC);

    state->mapped_overlay.is_mapped = TRUE;
}

static void unmap_overlay_file(CairoOverlayState *state) {
    if (state->mapped_overlay.is_mapped) {
        munmap(state->mapped_overlay.data, state->mapped_overlay.size);
        flock(state->mapped_overlay.fd, LOCK_UN);
        close(state->mapped_overlay.fd);
        state->mapped_overlay.is_mapped = FALSE;
    }
}


static void copy_overlay_data(CairoOverlayState *state) {
    if (!state->mapped_overlay.is_mapped || state->mapped_overlay.fd < 0) {
        return;
    }

    // Lock the file for reading
    if (flock(state->mapped_overlay.fd, LOCK_EX) == -1) {
        perror("Error locking file");
        return;
    }

    // Ensure the data is synchronized
    msync(state->mapped_overlay.data, BUFFER_WIDTH * BUFFER_HEIGHT * 4, MS_SYNC);

    // Copying data with fixed size
    memcpy(buffer, state->mapped_overlay.data, BUFFER_WIDTH * BUFFER_HEIGHT * 4);

    // Unlock the file
    flock(state->mapped_overlay.fd, LOCK_UN);
}


int main(int argc, char **argv) {
    GMainLoop *loop;
    GstElement *pipeline;
    GstBus *bus;
    CairoOverlayState *overlay_state;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    overlay_state = g_new0(CairoOverlayState, 1);
    map_overlay_file(overlay_state, OVERLAY_FILE);

    // Initialize the surface
    overlay_state->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BUFFER_WIDTH, BUFFER_HEIGHT);

    // Check if mapping was successful
    if (!overlay_state->mapped_overlay.is_mapped) {
        g_critical("Failed to map overlay file. Exiting.");
        g_free(overlay_state);
        return -1;
    }

    pipeline = setup_gst_pipeline(overlay_state);

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(on_message), loop);
    gst_object_unref(GST_OBJECT(bus));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (overlay_state->surface != NULL) {
        cairo_surface_destroy(overlay_state->surface);
    }

    unmap_overlay_file(overlay_state);

    g_free(overlay_state);
    return 0;
}
