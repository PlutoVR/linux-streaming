// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2022-2023, PlutoVR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pipeline module ElectricMaple XR streaming solution
 * @author Ryan Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */

#include "em_stream_client.h"
#include "em_app_log.h"
#include "em_connection.h"
#include "gst_common.h" // for em_sample

#include "os/os_threading.h"

#include <gst/app/gstappsink.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglsyncmeta.h>
#include <gst/gst.h>
#include <gst/gstbus.h>
#include <gst/gstelement.h>
#include <gst/gstinfo.h>
#include <gst/gstmessage.h>
#include <gst/gstsample.h>
#include <gst/gstutils.h>
#include <gst/video/video-frame.h>

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct em_sc_sample
{
	struct em_sample base;
	GstSample *sample;
};

struct _EmStreamClient
{
	GMainLoop *loop;
	EmConnection *connection;
	GstElement *pipeline;
	GstGLDisplay *gst_gl_display;
	GstGLContext *gst_gl_context;
	GstGLContext *gst_gl_other_context;

	GstGLDisplay *display;

	/// Wrapped version of the android_main/render context
	GstGLContext *android_main_context;

	/// GStreamer-created EGL context for its own use
	GstGLContext *context;

	GstElement *appsink;

	GLenum frame_texture_target;
	GLenum texture_target;
	GLuint texture_id;

	int width;
	int height;

	struct
	{
		EGLDisplay display;
		EGLContext android_main_context;
		// 16x16 pbuffer surface
		EGLSurface surface;
	} egl;

	struct
	{
		EGLContext context;
		EGLSurface read_surface;
		EGLSurface draw_surface;
	} old_egl;

	struct os_thread_helper play_thread;

	// This mutex protects the EGL context below across main and gstgl threads
	struct os_mutex egl_lock;

	bool pipeline_is_running;
};

#if 0
G_DEFINE_TYPE(EmStreamClient, em_stream_client, G_TYPE_OBJECT);


enum
{
	// action signals
	// SIGNAL_CONNECT,
	// SIGNAL_DISCONNECT,
	// SIGNAL_SET_PIPELINE,
	// signals
	// SIGNAL_WEBSOCKET_CONNECTED,
	// SIGNAL_WEBSOCKET_FAILED,
	// SIGNAL_CONNECTED,
	// SIGNAL_STATUS_CHANGE,
	// SIGNAL_ON_NEED_PIPELINE,
	// SIGNAL_ON_DROP_PIPELINE,
	// SIGNAL_DISCONNECTED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum
{
	PROP_CONNECTION = 1,
	// PROP_STATUS,
	N_PROPERTIES
} EmStreamClientProperty;
#endif

// clang-format off
#define SINK_CAPS \
    "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), "              \
    "format = (string) RGBA, "                                          \
    "width = " GST_VIDEO_SIZE_RANGE ", "                                \
    "height = " GST_VIDEO_SIZE_RANGE ", "                               \
    "framerate = " GST_VIDEO_FPS_RANGE ", "                             \
    "texture-target = (string) { 2D, external-oes } "

// clang-format on

/*
 * callbacks
 */

static void
on_need_pipeline_cb(EmConnection *emconn, EmStreamClient *sc);

static void
on_drop_pipeline_cb(EmConnection *emconn, EmStreamClient *sc);

static void *
em_stream_client_thread_func(void *ptr);

/*
 * Helper functions
 */

static void
em_stream_client_egl_save(EmStreamClient *sc);

static void
em_stream_client_egl_restore(EmStreamClient *sc, EGLDisplay display);

static void
em_stream_client_set_connection(EmStreamClient *sc, EmConnection *connection);

/* GObject method implementations */

#if 0

static void
em_stream_client_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	switch ((EmStreamClientProperty)property_id) {

	case PROP_CONNECTION:
		em_stream_client_set_connection(EM_STREAM_CLIENT(object), EM_CONNECTION(g_value_get_object(value)));
		break;

	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
	}
}

static void
em_stream_client_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{

	switch ((EmStreamClientProperty)property_id) {
	case PROP_CONNECTION: g_value_set_object(value, EM_STREAM_CLIENT(object)->connection); break;

	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
	}
}

#endif

static void
em_stream_client_init(EmStreamClient *sc)
{
	ALOGI("%s: creating stuff", __FUNCTION__);

	memset(sc, 0, sizeof(EmStreamClient));
	sc->loop = g_main_loop_new(NULL, FALSE);
	os_mutex_init(&sc->egl_lock);
	g_assert(os_thread_helper_init(&sc->play_thread) >= 0);
	ALOGI("%s: done creating stuff", __FUNCTION__);
}
static void
em_stream_client_dispose(EmStreamClient *self)
{
	// May be called multiple times during destruction.
	// Stop things and clear ref counted things here.
	// EmStreamClient *self = EM_STREAM_CLIENT(object);
	em_stream_client_stop(self);
	g_clear_object(&self->loop);
	g_clear_object(&self->connection);
	gst_clear_object(&self->pipeline);
	gst_clear_object(&self->gst_gl_display);
	gst_clear_object(&self->gst_gl_context);
	gst_clear_object(&self->gst_gl_other_context);
	gst_clear_object(&self->display);
	gst_clear_object(&self->context);
	gst_clear_object(&self->appsink);
}

static void
em_stream_client_finalize(EmStreamClient *self)
{
	// only called once, after dispose
	// EmStreamClient *self = EM_STREAM_CLIENT(object);
	os_thread_helper_destroy(&self->play_thread);
	os_mutex_destroy(&self->egl_lock);
}

#if 0
static void
em_stream_client_class_init(EmStreamClientClass *klass)
{
	ALOGE("RYLIE: %s: Begin", __FUNCTION__);

	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

	gobject_class->dispose = em_stream_client_dispose;
	gobject_class->finalize = em_stream_client_finalize;

	// gobject_class->set_property = em_stream_client_set_property;
	// gobject_class->get_property = em_stream_client_get_property;

	/**
	 * EmStreamClient:connection:
	 *
	 * The websocket URI for the signaling server
	 */
	// g_object_class_install_property(
	//     gobject_class, PROP_CONNECTION,
	//     g_param_spec_object("connection", "Connection", "EmConnection object for XR streaming",
	//     EM_TYPE_CONNECTION,
	//                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	ALOGE("RYLIE: %s: End", __FUNCTION__);
}

#endif

/*
 * callbacks
 */

static GstBusSyncReply
bus_sync_handler_cb(GstBus *bus, GstMessage *msg, EmStreamClient *sc)
{
	// LOG_MSG(msg);

	/* Do not let GstGL retrieve the display handle on its own
	 * because then it believes it owns it and calls eglTerminate()
	 * when disposed */
	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
		const gchar *type;
		gst_message_parse_context_type(msg, &type);
		if (g_str_equal(type, GST_GL_DISPLAY_CONTEXT_TYPE)) {
			ALOGI("Got message: Need display context");
			g_autoptr(GstContext) context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
			gst_context_set_gl_display(context, sc->display);
			gst_element_set_context(GST_ELEMENT(msg->src), context);
		} else if (g_str_equal(type, "gst.gl.app_context")) {
			ALOGI("Got message: Need app context");
			g_autoptr(GstContext) app_context = gst_context_new("gst.gl.app_context", TRUE);
			GstStructure *s = gst_context_writable_structure(app_context);
			gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, sc->android_main_context, NULL);
			gst_element_set_context(GST_ELEMENT(msg->src), app_context);
		}
	}

	return GST_BUS_PASS;
}

static gboolean
gst_bus_cb(GstBus *bus, GstMessage *message, gpointer data)
{
	// LOG_MSG(message);

	GstBin *pipeline = GST_BIN(data);

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *gerr = NULL;
		gchar *debug_msg = NULL;
		gst_message_parse_error(message, &gerr, &debug_msg);
		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-error");
		gchar *dotdata = gst_debug_bin_to_dot_data(pipeline, GST_DEBUG_GRAPH_SHOW_ALL);
		ALOGE("gst_bus_cb: DOT data: %s", dotdata);

		ALOGE("gst_bus_cb: Error: %s (%s)", gerr->message, debug_msg);
		g_error("gst_bus_cb: Error: %s (%s)", gerr->message, debug_msg);
		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_WARNING: {
		GError *gerr = NULL;
		gchar *debug_msg = NULL;
		gst_message_parse_warning(message, &gerr, &debug_msg);
		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-warning");
		ALOGW("gst_bus_cb: Warning: %s (%s)", gerr->message, debug_msg);
		g_warning("gst_bus_cb: Warning: %s (%s)", gerr->message, debug_msg);
		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_EOS: {
		g_error("gst_bus_cb: Got EOS!!");
	} break;
	default: break;
	}
	return TRUE;
}

static void
on_need_pipeline_cb(EmConnection *emconn, EmStreamClient *sc)
{
	g_assert_nonnull(sc);
	g_assert_nonnull(emconn);
	GError *error = NULL;


	// decodebin3 seems to .. hang?
	// omxh264dec doesn't seem to exist

	uint32_t width = 480;
	uint32_t height = 270;

	// We'll need an active egl context below before setting up gstgl (as explained previously)
	if (!em_stream_client_egl_begin_pbuffer(sc)) {
		ALOGE("%s: Failed to make EGL context current, cannot create pipeline!", __FUNCTION__);
		return;
	}

	gchar *pipeline_string = g_strdup_printf(
	    "webrtcbin name=webrtc bundle-policy=max-bundle ! "
	    "rtph264depay ! "
	    "h264parse ! "
	    "video/x-h264,stream-format=(string)byte-stream, alignment=(string)au,parsed=(boolean)true !"
	    "amcviddec-omxqcomvideodecoderavc ! "
	    "glsinkbin name=glsink");

	sc->pipeline = gst_object_ref_sink(gst_parse_launch(pipeline_string, &error));
	if (sc->pipeline == NULL) {
		ALOGE("FRED: Failed creating pipeline : Bad source: %s", error->message);
		abort();
	}

	// Un-current the EGL context
	em_stream_client_egl_end(sc);

	// We convert the string SINK_CAPS above into a GstCaps that elements below can understand.
	// the "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY ")," part of the caps is read :
	// video/x-raw(memory:GLMemory) and is really important for getting zero-copy gl textures.
	// It tells the pipeline (especially the decoder) that an internal android:Surface should
	// get created internally (using the provided gstgl contexts above) so that the appsink
	// can basically pull the samples out using an GLConsumer (this is just for context, as
	// all of those constructs will be hidden from you, but are turned on by that CAPS).
	g_autoptr(GstCaps) caps = gst_caps_from_string(SINK_CAPS);

	// FRED: We create the appsink 'manually' here because glsink's ALREADY a sink and so if we stick
	//       glsinkbin ! appsink in our pipeline_string for automatic linking, gst_parse will NOT like this,
	//       as glsinkbin (a sink) cannot link to anything upstream (appsink being 'another' sink). So we
	//       manually link them below using glsinkbin's 'sink' pad -> appsink.
	sc->appsink = gst_element_factory_make("appsink", NULL);
	g_object_set(sc->appsink,
	             // Set caps
	             "caps", caps,
	             // Fixed size buffer
	             "max-buffers", 1,
	             // drop old buffers when queue is filled
	             "drop", true,
	             // terminator
	             NULL);
	g_autoptr(GstElement) glsinkbin = gst_bin_get_by_name(GST_BIN(sc->pipeline), "glsink");
	g_object_set(glsinkbin, "sink", sc->appsink, NULL);

	g_autoptr(GstBus) bus = gst_element_get_bus(sc->pipeline);
	// We set this up to inject the EGL context
	gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler_cb, sc, NULL);

	// This just watches for errors and such
	gst_bus_add_watch(bus, gst_bus_cb, sc->pipeline);
	g_object_unref(bus);

	sc->pipeline_is_running = TRUE;

	// This actually hands over the pipeline. Once our own handler returns, the pipeline will be started by the
	// connection.
	g_signal_emit_by_name(emconn, "set-pipeline", GST_PIPELINE(sc->pipeline), NULL);
}

static void
on_drop_pipeline_cb(EmConnection *emconn, EmStreamClient *sc)
{
	if (sc->pipeline) {
		gst_element_set_state(sc->pipeline, GST_STATE_NULL);
	}
	gst_clear_object(&sc->pipeline);
	gst_clear_object(&sc->appsink);
}

static void *
em_stream_client_thread_func(void *ptr)
{

	EmStreamClient *sc = (EmStreamClient *)ptr;

	ALOGI("%s: running GMainLoop", __FUNCTION__);
	ALOGD("em_fs_mainloop: running GMainLoop!");
	g_main_loop_run(sc->loop);
	ALOGI("%s: g_main_loop_run returned", __FUNCTION__);

	return NULL;
}

/*
 * Public functions
 */
EmStreamClient *
em_stream_client_new()
{
#if 0
	ALOGI("%s: before g_object_new", __FUNCTION__);
	gpointer self_untyped = g_object_new(EM_TYPE_STREAM_CLIENT, NULL);
	if (self_untyped == NULL) {
		ALOGE("%s: g_object_new failed to allocate", __FUNCTION__);
		return NULL;
	}
	EmStreamClient *self = EM_STREAM_CLIENT(self_untyped);

	ALOGI("%s: after g_object_new", __FUNCTION__);
#endif
	EmStreamClient *self = calloc(1, sizeof(EmStreamClient));
	em_stream_client_init(self);
	return self;
}

void
em_stream_client_destroy(EmStreamClient **ptr_sc)
{
	if (ptr_sc == NULL) {
		return;
	}
	EmStreamClient *sc = *ptr_sc;
	if (sc == NULL) {
		return;
	}
	em_stream_client_dispose(sc);
	em_stream_client_finalize(sc);
	free(sc);
	*ptr_sc = NULL;
}

void
em_stream_client_set_egl_context(EmStreamClient *sc, EGLDisplay display, EGLContext context, EGLSurface pbuffer_surface)
{

	ALOGI("RYLIE: wrapping egl context");
	em_stream_client_egl_save(sc);

	ALOGV("RYLIE: eglMakeCurrent make-current");

	if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_FALSE) {
		ALOGV("RYLIE: em_stream_client_set_egl_context: Failed to make egl context current");
		return;
	}
	sc->egl.display = display;
	sc->egl.android_main_context = context;
	sc->egl.surface = pbuffer_surface;

	const GstGLPlatform egl_platform = GST_GL_PLATFORM_EGL;
	guintptr android_main_egl_context_handle = gst_gl_context_get_current_gl_context(egl_platform);
	GstGLAPI gl_api = gst_gl_context_get_current_gl_api(egl_platform, NULL, NULL);
	sc->gst_gl_display = g_object_ref_sink(gst_gl_display_new());
	sc->android_main_context = g_object_ref_sink(
	    gst_gl_context_new_wrapped(sc->gst_gl_display, android_main_egl_context_handle, egl_platform, gl_api));

	ALOGV("RYLIE: eglMakeCurrent un-make-current");
	em_stream_client_egl_restore(sc, display);
}

void
em_stream_client_egl_mutex_lock(EmStreamClient *sc)
{
	os_mutex_lock(&sc->egl_lock);
}

void
em_stream_client_egl_mutex_unlock(EmStreamClient *sc)
{
	os_mutex_unlock(&sc->egl_lock);
}

bool
em_stream_client_egl_begin(EmStreamClient *sc, EGLSurface draw, EGLSurface read)
{
	em_stream_client_egl_mutex_lock(sc);
	em_stream_client_egl_save(sc);
	ALOGE("%s : make current display=%p, draw surface=%p, read surface=%p, context=%p", __FUNCTION__,
	      sc->egl.display, draw, read, sc->egl.android_main_context);
	if (eglMakeCurrent(sc->egl.display, draw, read, sc->egl.android_main_context) == EGL_FALSE) {
		ALOGE("%s: Failed make egl context current", __FUNCTION__);
		em_stream_client_egl_mutex_unlock(sc);
		return false;
	}
	return true;
}

bool
em_stream_client_egl_begin_pbuffer(EmStreamClient *sc)
{
	return em_stream_client_egl_begin(sc, sc->egl.surface, sc->egl.surface);
}

void
em_stream_client_egl_end(EmStreamClient *sc)
{
	ALOGI("%s: Make egl context un-current", __FUNCTION__);
	em_stream_client_egl_restore(sc, sc->egl.display);
	em_stream_client_egl_mutex_unlock(sc);
}

void
em_stream_client_spawn_thread(EmStreamClient *sc, EmConnection *connection)
{
	ALOGI("%s: Starting stream client mainloop thread", __FUNCTION__);
	em_stream_client_set_connection(sc, connection);
	int ret = os_thread_helper_start(&sc->play_thread, &em_stream_client_thread_func, sc);
	(void)ret;
	g_assert(ret == 0);
}

void
em_stream_client_stop(EmStreamClient *sc)
{
	ALOGI("%s: Stopping pipeline and ending thread", __FUNCTION__);

	if (sc->pipeline != NULL) {
		gst_element_set_state(sc->pipeline, GST_STATE_NULL);
		os_thread_helper_stop_and_wait(&sc->play_thread);
	}
	gst_clear_object(&sc->pipeline);
	gst_clear_object(&sc->appsink);
	gst_clear_object(&sc->context);

	sc->pipeline_is_running = false;
}

struct em_sample *
em_stream_client_try_pull_sample(EmStreamClient *sc)
{

	// FOR RYAN : As mentioned, the glsinkbin -> appsink part of the gstreamer pipeline in gst_driver.c
	// will output "samples" that might be signalled for using "new_sample_cb" in gst_driver (useful for
	// testing if the gstreamer sink elements are giving us "anything"), but also on which we can "poll"
	// to see and that's what we do here. Note that the non-try version of the call "gst_app_sink_pull_sample"
	// WILL BLOCK until there's a sample.

	// Get Newest sample from GST appsink. Waiting 1ms here before giving up (might want to adjust that time)
	// ALOGE("DEBUG: Trying to get new gstgl sample, waiting max 1ms\n");

	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sc->appsink), (GstClockTime)(1000 * GST_USECOND));

	if (sample == NULL) {
		return NULL;
	}

	ALOGE("FRED: GOT A SAMPLE !!!");
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);

	GstVideoInfo info;
	gst_video_info_from_caps(&info, caps);
	/*gint width = GST_VIDEO_INFO_WIDTH (&info);
	gint height = GST_VIDEO_INFO_HEIGHT (&info);*/

	// FOR RYAN: Handle resize according to how it's done in PlutosphereOXR
	/*if (width != sc->width || height != sc->height) {
	    sc->width = width;
	    sc->height = height;
	}*/

	struct em_sc_sample *ret = calloc(1, sizeof(struct em_sc_sample));

	GstVideoFrame frame;
	GstMapFlags flags = (GstMapFlags)(GST_MAP_READ | GST_MAP_GL);
	gst_video_frame_map(&frame, &info, buffer, flags);
	ret->base.frame_texture_id = *(GLuint *)frame.data[0];

	if (sc->context == NULL) {
		ALOGI("%s: Retrieving the GStreamer EGL context", __FUNCTION__);
		/* Get GStreamer's gl context. */
		gst_gl_query_local_gl_context(sc->appsink, GST_PAD_SINK, &sc->context);

		/* Check if we have 2D or OES textures */
		GstStructure *s = gst_caps_get_structure(caps, 0);
		const gchar *texture_target_str = gst_structure_get_string(s, "texture-target");
		if (g_str_equal(texture_target_str, GST_GL_TEXTURE_TARGET_EXTERNAL_OES_STR)) {
			sc->frame_texture_target = GL_TEXTURE_EXTERNAL_OES;
		} else if (g_str_equal(texture_target_str, GST_GL_TEXTURE_TARGET_2D_STR)) {
			sc->frame_texture_target = GL_TEXTURE_2D;
			ALOGE("RYLIE: Got GL_TEXTURE_2D instead of expected GL_TEXTURE_EXTERNAL_OES");
		} else {
			g_assert_not_reached();
		}
	}
	ret->base.frame_texture_target = sc->frame_texture_target;

	GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(buffer);
	if (sync_meta) {
		/* MOSHI: the set_sync() seems to be needed for resizing */
		gst_gl_sync_meta_set_sync_point(sync_meta, sc->context);
		gst_gl_sync_meta_wait(sync_meta, sc->context);
	}

	gst_video_frame_unmap(&frame);
	// move sample ownership into the return value
	ret->sample = sample;
	return &(ret->base);
}

void
em_stream_client_release_sample(EmStreamClient *sc, struct em_sample *ems)
{

	struct em_sc_sample *impl = (struct em_sc_sample *)ems;
	ALOGW("RYLIE: Releasing sample with texture ID %d", ems->frame_texture_id);
	gst_sample_unref(impl->sample);
	free(impl);
}


/*
 * Helper functions
 */


static void
em_stream_client_egl_save(EmStreamClient *sc)
{
	sc->old_egl.context = eglGetCurrentContext();
	sc->old_egl.read_surface = eglGetCurrentSurface(EGL_READ);
	sc->old_egl.draw_surface = eglGetCurrentSurface(EGL_DRAW);
}

static void
em_stream_client_egl_restore(EmStreamClient *sc, EGLDisplay display)
{
	eglMakeCurrent(display, sc->old_egl.draw_surface, sc->old_egl.read_surface, sc->old_egl.context);
}

static void
em_stream_client_set_connection(EmStreamClient *sc, EmConnection *connection)
{
	g_clear_object(&sc->connection);
	if (connection != NULL) {
		sc->connection = g_object_ref(connection);
		g_signal_connect(sc->connection, "on-need-pipeline", G_CALLBACK(on_need_pipeline_cb), sc);
		g_signal_connect(sc->connection, "on-drop-pipeline", G_CALLBACK(on_drop_pipeline_cb), sc);
		ALOGI("%s: EmConnection assigned", __FUNCTION__);
	}
}