// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A GStreamer pipeline for WebRTC streaming
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakub Adam <jakub.adam@collabora.com
 * @author Nicolas Dufresne <nicolas.dufresne@collabora.com>
 * @author Olivier Crête <olivier.crete@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Aaron Boxer <aaron.boxer@collabora.com>
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup aux_util
 */

#include "ems_gstreamer_pipeline.h"

#include "ems_callbacks.h"

#include "os/os_threading.h"
#include "util/u_misc.h"
#include "util/u_debug.h"

#include "pb_decode.h"
#include "electricmaple.pb.h"

// Monado includes
#include "gstreamer/gst_internal.h"
#include "gstreamer/gst_pipeline.h"

#include "ems_signaling_server.h"

#include <glib-unix.h>
#include <gst/gst.h>
#include <gst/gststructure.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/datachannel.h>
#include <gst/webrtc/rtcsessiondescription.h>
#undef GST_USE_UNSTABLE_API

#include <stdio.h>
#include <assert.h>

#define WEBRTC_TEE_NAME "webrtctee"

#ifdef __aarch64__
#define DEFAULT_VIDEOSINK " queue max-size-bytes=0 ! kmssink bus-id=a0070000.v_mix"
#else
#define DEFAULT_VIDEOSINK " videoconvert ! autovideosink "
#endif



EmsSignalingServer *signaling_server;


struct ems_gstreamer_pipeline
{
	struct gstreamer_pipeline base;


	// struct GstElement *pipeline;
	GstElement *webrtc;

	GObject *data_channel;
	guint timeout_src_id;


	struct ems_callbacks *callbacks;
};


static gboolean
sigint_handler(gpointer user_data)
{
	g_main_loop_quit(user_data);
	return G_SOURCE_REMOVE;
}

static gboolean
gst_bus_cb(GstBus *bus, GstMessage *message, gpointer user_data)
{
	struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)user_data;
	GstBin *pipeline = GST_BIN(egp->base.pipeline);

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *gerr;
		gchar *debug_msg;
		gst_message_parse_error(message, &gerr, &debug_msg);
		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "mss-pipeline-ERROR");
		g_error("Error: %s (%s)", gerr->message, debug_msg);
		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_WARNING: {
		GError *gerr;
		gchar *debug_msg;
		gst_message_parse_warning(message, &gerr, &debug_msg);
		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "mss-pipeline-WARNING");
		g_warning("Warning: %s (%s)", gerr->message, debug_msg);
		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_EOS: {
		g_error("Got EOS!!");
	} break;
	default: break;
	}
	return TRUE;
}

static GstElement *
get_webrtcbin_for_client(GstBin *pipeline, EmsClientId client_id)
{
	gchar *name;
	GstElement *webrtcbin;

	name = g_strdup_printf("webrtcbin_%p", client_id);
	webrtcbin = gst_bin_get_by_name(pipeline, name);
	g_free(name);

	return webrtcbin;
}

static void
connect_webrtc_to_tee(GstElement *webrtcbin)
{
	GstElement *pipeline;
	GstElement *tee;
	GstPad *srcpad;
	GstPad *sinkpad;
	GstPadLinkReturn ret;

	pipeline = GST_ELEMENT(gst_element_get_parent(webrtcbin));
	if (pipeline == NULL)
		return;
	tee = gst_bin_get_by_name(GST_BIN(pipeline), WEBRTC_TEE_NAME);
	srcpad = gst_element_request_pad_simple(tee, "src_%u");
	sinkpad = gst_element_request_pad_simple(webrtcbin, "sink_0");
	ret = gst_pad_link(srcpad, sinkpad);
	g_assert(ret == GST_PAD_LINK_OK);
	gst_object_unref(srcpad);
	gst_object_unref(sinkpad);
	gst_object_unref(tee);
	gst_object_unref(pipeline);
}

static void
on_offer_created(GstPromise *promise, GstElement *webrtcbin)
{
	GstWebRTCSessionDescription *offer = NULL;
	gchar *sdp;

	gst_structure_get(gst_promise_get_reply(promise), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
	gst_promise_unref(promise);

	g_signal_emit_by_name(webrtcbin, "set-local-description", offer, NULL);

	sdp = gst_sdp_message_as_text(offer->sdp);
	ems_signaling_server_send_sdp_offer(signaling_server, g_object_get_data(G_OBJECT(webrtcbin), "client_id"), sdp);
	g_free(sdp);

	gst_webrtc_session_description_free(offer);

	connect_webrtc_to_tee(webrtcbin);
}

static void
webrtc_on_data_channel_cb(GstElement *webrtcbin, GObject *data_channel, struct ems_gstreamer_pipeline *egp)
{
	U_LOG_I("called!!!!");
}



static void
webrtc_on_ice_candidate_cb(GstElement *webrtcbin, guint mlineindex, gchar *candidate)
{
	ems_signaling_server_send_candidate(signaling_server, g_object_get_data(G_OBJECT(webrtcbin), "client_id"),
	                                    mlineindex, candidate);
}


static void
data_channel_error_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp)
{
	U_LOG_E("error");
}

gboolean
datachannel_send_message(GstWebRTCDataChannel *datachannel)
{
	g_signal_emit_by_name(datachannel, "send-string", "Hi! from Electric Maple Server");

	char buf[] = "Electric Maple Server";
	GBytes *b = g_bytes_new_static(buf, ARRAY_SIZE(buf));
	gst_webrtc_data_channel_send_data(datachannel, b);

	return G_SOURCE_CONTINUE;
}

static void
data_channel_open_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp)
{
	U_LOG_I("data channel opened");

	egp->timeout_src_id = g_timeout_add_seconds(3, G_SOURCE_FUNC(datachannel_send_message), datachannel);
}

static void
data_channel_close_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp)
{
	U_LOG_I("data channel closed");

	g_clear_handle_id(&egp->timeout_src_id, g_source_remove);
	g_clear_object(&egp->data_channel);
}

static void
data_channel_message_data_cb(GstWebRTCDataChannel *datachannel, GBytes *data, struct ems_gstreamer_pipeline *egp)
{
	em_proto_UpMessage message = em_proto_UpMessage_init_default;
	size_t n = 0;

	const unsigned char *buf = (const unsigned char *)g_bytes_get_data(data, &n);
	pb_istream_t our_istream = pb_istream_from_buffer(buf, n);

	bool result = pb_decode_ex(&our_istream, &em_proto_UpMessage_msg, &message, PB_DECODE_NULLTERMINATED);

	if (!result) {
		U_LOG_E("Error! %s", PB_GET_ERROR(&our_istream));
		return;
	}
	ems_callbacks_call(egp->callbacks, EMS_CALLBACKS_EVENT_TRACKING, &message);
}

static void
data_channel_message_string_cb(GstWebRTCDataChannel *datachannel, gchar *str, struct ems_gstreamer_pipeline *egp)
{
	U_LOG_I("Received data channel message: %s\n", str);
}


static void
webrtc_client_connected_cb(EmsSignalingServer *server, EmsClientId client_id, struct ems_gstreamer_pipeline *egp)
{
	GstBin *pipeline = GST_BIN(egp->base.pipeline);
	gchar *name;
	GstElement *webrtcbin;
	GstCaps *caps;
	GstStateChangeReturn ret;
	GstWebRTCRTPTransceiver *transceiver;

	name = g_strdup_printf("webrtcbin_%p", client_id);

	webrtcbin = gst_element_factory_make("webrtcbin", name);
	g_object_set(webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
	g_object_set_data(G_OBJECT(webrtcbin), "client_id", client_id);
	gst_bin_add(pipeline, webrtcbin);

	ret = gst_element_set_state(webrtcbin, GST_STATE_READY);
	g_assert(ret != GST_STATE_CHANGE_FAILURE);

	g_signal_connect(webrtcbin, "on-data-channel", G_CALLBACK(webrtc_on_data_channel_cb), NULL);

	// I also think this would work if the pipeline state is READY but /shrug

	// TODO add priority
	GstStructure *data_channel_options = gst_structure_new_from_string("data-channel-options, ordered=true");
	g_signal_emit_by_name(webrtcbin, "create-data-channel", "channel", data_channel_options, &egp->data_channel);
	gst_clear_structure(&data_channel_options);

	if (!egp->data_channel) {
		U_LOG_E("Couldn't make datachannel!");
		assert(false);
	} else {
		U_LOG_I("Successfully created datachannel!");

		g_signal_connect(egp->data_channel, "on-open", G_CALLBACK(data_channel_open_cb), egp);
		g_signal_connect(egp->data_channel, "on-close", G_CALLBACK(data_channel_close_cb), egp);
		g_signal_connect(egp->data_channel, "on-error", G_CALLBACK(data_channel_error_cb), egp);
		g_signal_connect(egp->data_channel, "on-message-data", G_CALLBACK(data_channel_message_data_cb), egp);
		g_signal_connect(egp->data_channel, "on-message-string", G_CALLBACK(data_channel_message_string_cb),
		                 egp);
	}

	ret = gst_element_set_state(webrtcbin, GST_STATE_PLAYING);
	g_assert(ret != GST_STATE_CHANGE_FAILURE);

	g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(webrtc_on_ice_candidate_cb), NULL);

	caps = gst_caps_from_string(
	    "application/x-rtp, "
	    "payload=96,encoding-name=H264,clock-rate=90000,media=video,packetization-mode=(string)1,profile-level-id=("
	    "string)42e01f");
	g_signal_emit_by_name(webrtcbin, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, caps,
	                      &transceiver);

	gst_caps_unref(caps);
	gst_clear_object(&transceiver);

	g_signal_emit_by_name(
	    webrtcbin, "create-offer", NULL,
	    gst_promise_new_with_change_func((GstPromiseChangeFunc)on_offer_created, webrtcbin, NULL));

	GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "rtcbin");

	g_free(name);
}

static void
webrtc_sdp_answer_cb(EmsSignalingServer *server,
                     EmsClientId client_id,
                     const gchar *sdp,
                     struct ems_gstreamer_pipeline *egp)
{
	GstBin *pipeline = GST_BIN(egp->base.pipeline);
	GstSDPMessage *sdp_msg = NULL;
	GstWebRTCSessionDescription *desc = NULL;

	if (gst_sdp_message_new_from_text(sdp, &sdp_msg) != GST_SDP_OK) {
		g_debug("Error parsing SDP description");
		goto out;
	}

	desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
	if (desc) {
		GstElement *webrtcbin;
		GstPromise *promise;

		webrtcbin = get_webrtcbin_for_client(pipeline, client_id);
		if (!webrtcbin) {
			goto out;
		}
		promise = gst_promise_new();

		g_signal_emit_by_name(webrtcbin, "set-remote-description", desc, promise);

		gst_promise_wait(promise);
		gst_promise_unref(promise);

		gst_object_unref(webrtcbin);
	} else {
		gst_sdp_message_free(sdp_msg);
	}

out:
	g_clear_pointer(&desc, gst_webrtc_session_description_free);
}

static void
webrtc_candidate_cb(EmsSignalingServer *server,
                    EmsClientId client_id,
                    guint mlineindex,
                    const gchar *candidate,
                    struct ems_gstreamer_pipeline *egp)
{
	GstBin *pipeline = GST_BIN(egp->base.pipeline);

	if (strlen(candidate)) {
		GstElement *webrtcbin;

		webrtcbin = get_webrtcbin_for_client(pipeline, client_id);
		if (webrtcbin) {
			g_signal_emit_by_name(webrtcbin, "add-ice-candidate", mlineindex, candidate);
			gst_object_unref(webrtcbin);
		}
	}

	g_debug("Remote candidate: %s", candidate);
}

static GstPadProbeReturn
remove_webrtcbin_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	GstElement *webrtcbin = GST_ELEMENT(user_data);

	gst_bin_remove(GST_BIN(GST_ELEMENT_PARENT(webrtcbin)), webrtcbin);
	gst_element_set_state(webrtcbin, GST_STATE_NULL);

	return GST_PAD_PROBE_REMOVE;
}

static void
webrtc_client_disconnected_cb(EmsSignalingServer *server, EmsClientId client_id, struct ems_gstreamer_pipeline *egp)
{
	GstBin *pipeline = GST_BIN(egp->base.pipeline);
	GstElement *webrtcbin;

	webrtcbin = get_webrtcbin_for_client(pipeline, client_id);

	if (webrtcbin) {
		GstPad *sinkpad;

		sinkpad = gst_element_get_static_pad(webrtcbin, "sink_0");
		if (sinkpad) {
			gst_pad_add_probe(GST_PAD_PEER(sinkpad), GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
			                  remove_webrtcbin_probe_cb, webrtcbin, gst_object_unref);

			gst_clear_object(&sinkpad);
		}
	}
}

struct RestartData
{
	GstElement *src;
	GstElement *pipeline;
};

static void
free_restart_data(gpointer user_data)
{
	struct RestartData *rd = user_data;

	gst_object_unref(rd->src);
	g_free(rd);
}

static gboolean
restart_source(gpointer user_data)
{
	struct RestartData *rd = user_data;
	GstElement *e;
	GstStateChangeReturn ret;

	gst_element_set_state(rd->src, GST_STATE_NULL);
	gst_element_set_locked_state(rd->src, TRUE);
	e = gst_bin_get_by_name(GST_BIN(rd->pipeline), "srtqueue");
	gst_bin_add(GST_BIN(rd->pipeline), rd->src);
	if (!gst_element_link(rd->src, e))
		g_assert_not_reached();
	gst_element_set_locked_state(rd->src, FALSE);
	ret = gst_element_set_state(rd->src, GST_STATE_PLAYING);
	g_assert(ret != GST_STATE_CHANGE_FAILURE);
	gst_object_unref(e);

	g_debug("Restarted source after EOS");

	return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
src_event_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	GstElement *pipeline = user_data;
	GstElement *src;
	struct RestartData *rd;

	if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS)
		return GST_PAD_PROBE_PASS;

	src = gst_pad_get_parent_element(pad);

	gst_bin_remove(GST_BIN(pipeline), src);

	rd = g_new(struct RestartData, 1);
	rd->src = src;
	rd->pipeline = pipeline;
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, restart_source, rd, free_restart_data);

	return GST_PAD_PROBE_DROP;
}

static gboolean
print_stats(gpointer user_data)
{
	GstElement *src = user_data;
	GstStructure *s;
	char *str;

	g_object_get(src, "stats", &s, NULL);
	str = gst_structure_to_string(s);
	// g_debug ("%s", str);
	g_free(str);
	gst_structure_free(s);

	return G_SOURCE_CONTINUE;
}


/*
 *
 * Internal pipeline functions.
 *
 */

static void
break_apart(struct xrt_frame_node *node)
{
	struct gstreamer_pipeline *gp = container_of(node, struct gstreamer_pipeline, node);

	/*
	 * This function is called when we are shutting down, after returning
	 * from this function you are not allowed to call any other nodes in the
	 * graph. But it must be safe for other nodes to call any normal
	 * functions on us. Once the context is done calling break_aprt on all
	 * objects it will call destroy on them.
	 */

	(void)gp;
}

static void
destroy(struct xrt_frame_node *node)
{
	struct gstreamer_pipeline *gp = container_of(node, struct gstreamer_pipeline, node);

	/*
	 * All of the nodes has been broken apart and none of our functions will
	 * be called, it's now safe to destroy and free ourselves.
	 */

	free(gp);
}

GMainLoop *main_loop;

void *
loop_thread(void *data)
{
	g_main_loop_run(main_loop);
	return NULL;
}


/*
 *
 * Exported functions.
 *
 */

void
ems_gstreamer_pipeline_play(struct gstreamer_pipeline *gp)
{
	U_LOG_I("Starting pipeline");
	struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)gp;

	main_loop = g_main_loop_new(NULL, FALSE);


	GstStateChangeReturn ret = gst_element_set_state(egp->base.pipeline, GST_STATE_PLAYING);

	g_assert(ret != GST_STATE_CHANGE_FAILURE);

	g_signal_connect(signaling_server, "ws-client-connected", G_CALLBACK(webrtc_client_connected_cb), egp);

	pthread_t thread;
	pthread_create(&thread, NULL, loop_thread, NULL);
}

void
ems_gstreamer_pipeline_stop(struct gstreamer_pipeline *gp)
{
	struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)gp;
	U_LOG_I("Stopping pipeline");

	// Settle the pipeline.
	U_LOG_T("Sending EOS");
	gst_element_send_event(egp->base.pipeline, gst_event_new_eos());

	// Wait for EOS message on the pipeline bus.
	U_LOG_T("Waiting for EOS");
	GstMessage *msg = NULL;
	msg = gst_bus_timed_pop_filtered(GST_ELEMENT_BUS(egp->base.pipeline), GST_CLOCK_TIME_NONE,
	                                 GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	//! @todo Should check if we got an error message here or an eos.
	(void)msg;

	// Completely stop the pipeline.
	U_LOG_T("Setting to NULL");
	gst_element_set_state(egp->base.pipeline, GST_STATE_NULL);
}



void
ems_gstreamer_pipeline_create(struct xrt_frame_context *xfctx,
                              const char *appsrc_name,
                              struct ems_callbacks *callbacks_collection,
                              struct gstreamer_pipeline **out_gp)
{
	gchar *pipeline_str;
	GstElement *pipeline;
	GError *error = NULL;
	GstBus *bus;


	signaling_server = ems_signaling_server_new();

	pipeline_str = g_strdup_printf(
	    "appsrc name=%s ! "                //
	    "queue ! "                         //
	    "videoconvert ! "                  //
	    "video/x-raw,format=NV12 ! "       //
	    "queue !"                          //
	    "x264enc tune=zerolatency ! "      //
	    "video/x-h264,profile=baseline ! " //
	    "queue !"                          //
	    "h264parse ! "                     //
	    "rtph264pay config-interval=1 ! "  //
	    "application/x-rtp,payload=96 ! "  //
	    "tee name=%s allow-not-linked=true",
	    appsrc_name, WEBRTC_TEE_NAME);

	// no webrtc bin yet until later!

	printf("%s\n\n\n\n", pipeline_str);

	struct ems_gstreamer_pipeline *egp = U_TYPED_CALLOC(struct ems_gstreamer_pipeline);
	egp->base.node.break_apart = break_apart;
	egp->base.node.destroy = destroy;
	egp->base.xfctx = xfctx;
	egp->callbacks = callbacks_collection;


	gst_init(NULL, NULL);

	pipeline = gst_parse_launch(pipeline_str, &error);
	g_assert_no_error(error);
	g_free(pipeline_str);

	bus = gst_element_get_bus(pipeline);
	gst_bus_add_watch(bus, gst_bus_cb, egp);
	gst_object_unref(bus);

	g_signal_connect(signaling_server, "ws-client-disconnected", G_CALLBACK(webrtc_client_disconnected_cb), egp);
	g_signal_connect(signaling_server, "sdp-answer", G_CALLBACK(webrtc_sdp_answer_cb), egp);
	g_signal_connect(signaling_server, "candidate", G_CALLBACK(webrtc_candidate_cb), egp);

	// loop = g_main_loop_new (NULL, FALSE);
	// g_unix_signal_add (SIGINT, sigint_handler, loop);

	g_print(
	    "Output streams:\n"
	    "\tWebRTC: http://127.0.0.1:8080\n");

	// Setup pipeline.
	egp->base.pipeline = pipeline;
	// GstElement *appsrc = gst_element_factory_make("appsrc", appsrc_name);
	// GstElement *conv = gst_element_factory_make("videoconvert", "conv");
	// GstElement *scale = gst_element_factory_make("videoscale", "scale");
	// GstElement *videosink = gst_element_factory_make("autovideosink", "videosink");


	/*
	 * Add ourselves to the context so we are destroyed.
	 * This is done once we know everything is completed.
	 */
	xrt_frame_context_add(xfctx, &egp->base.node);

	*out_gp = &egp->base;
}
