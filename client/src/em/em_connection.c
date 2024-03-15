// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2022-2023, PlutoVR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  WebRTC handshake/connection for ElectricMaple XR streaming frameserver
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @ingroup em_client
 */

#include "em_connection.h"

#include "em_status.h"
#include "em_app_log.h"

#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <stdbool.h>
#include <string.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#undef GST_USE_UNSTABLE_API

#include <libsoup/soup-message.h>
#include <libsoup/soup-session.h>

#include <json-glib/json-glib.h>

/*!
 * Data required for the handshake to complete and to maintain the connection.
 */
struct _EmConnection
{
	GObject parent;
	SoupSession *soup_session;
	gchar *websocket_uri;
	/// Cancellable for websocket connection process
	GCancellable *ws_cancel;
	SoupWebsocketConnection *ws;

	GstPipeline *pipeline;
	GstElement *webrtcbin;
	GstWebRTCDataChannel *datachannel;

	enum em_status status;
};


G_DEFINE_TYPE(EmConnection, em_connection, G_TYPE_OBJECT)

enum
{
	// action signals
	SIGNAL_CONNECT,
	SIGNAL_DISCONNECT,
	SIGNAL_SET_PIPELINE,
	// signals
	SIGNAL_WEBSOCKET_CONNECTED,
	SIGNAL_WEBSOCKET_FAILED,
	SIGNAL_CONNECTED,
	SIGNAL_STATUS_CHANGE,
	SIGNAL_ON_NEED_PIPELINE,
	SIGNAL_ON_DROP_PIPELINE,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum
{
	PROP_WEBSOCKET_URI = 1,
	// PROP_STATUS,
	N_PROPERTIES
} EmConnectionProperty;

static GParamSpec *properties[N_PROPERTIES] = {
    NULL,
};

#define DEFAULT_WEBSOCKET_URI "ws://127.0.0.1:8080/ws"


/* GObject method implementations */

static void
em_connection_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	EmConnection *self = EM_CONNECTION(object);

	switch ((EmConnectionProperty)property_id) {
	case PROP_WEBSOCKET_URI:
		g_free(self->websocket_uri);
		self->websocket_uri = g_value_dup_string(value);
		ALOGI("RYLIE: websocket URI assigned; %s", self->websocket_uri);
		break;


	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
	}
}

static void
em_connection_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	EmConnection *self = EM_CONNECTION(object);

	switch ((EmConnectionProperty)property_id) {
	case PROP_WEBSOCKET_URI: g_value_set_string(value, self->websocket_uri); break;

	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
	}
}

static void
em_connection_init(EmConnection *emconn)
{
	emconn->ws_cancel = g_cancellable_new();
	emconn->soup_session = soup_session_new();
	emconn->websocket_uri = g_strdup(DEFAULT_WEBSOCKET_URI);
}

static void
em_connection_dispose(GObject *object)
{
	EmConnection *self = EM_CONNECTION(object);

	em_connection_disconnect(self);

	g_clear_object(&self->soup_session);
	g_clear_object(&self->ws_cancel);
}

static void
em_connection_finalize(GObject *object)
{
	EmConnection *self = EM_CONNECTION(object);

	g_free(self->websocket_uri);
}

static void
em_connection_class_init(EmConnectionClass *klass)
{
	ALOGE("RYLIE: %s: Begin", __FUNCTION__);
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

	gobject_class->dispose = em_connection_dispose;
	gobject_class->finalize = em_connection_finalize;

	gobject_class->set_property = em_connection_set_property;
	gobject_class->get_property = em_connection_get_property;

	/**
	 * EmConnection:websocket-uri:
	 *
	 * The websocket URI for the signaling server
	 */
	g_object_class_install_property(
	    gobject_class, PROP_WEBSOCKET_URI,
	    g_param_spec_string("websocket-uri", "WebSocket URI", "WebSocket URI for signaling server.",
	                        DEFAULT_WEBSOCKET_URI /* default value */,
	                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	 * EmConnection::connect
	 * @object: the #EmConnection
	 *
	 * Start the connection process
	 */
	signals[SIGNAL_CONNECT] =
	    g_signal_new_class_handler("connect", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
	                               G_CALLBACK(em_connection_connect), NULL, NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * EmConnection::disconnect
	 * @object: the #EmConnection
	 *
	 * Stop the connection process or shutdown the connection
	 */
	signals[SIGNAL_DISCONNECT] =
	    g_signal_new_class_handler("disconnect", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
	                               G_CALLBACK(em_connection_disconnect), NULL, NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * EmConnection::set-pipeline
	 * @object: the #EmConnection
	 * @pipeline: A #GstPipeline
	 *
	 * Sets the #GstPipeline containing a #GstWebRTCBin element and begins the WebRTC connection negotiation.
	 * Should be signalled in response to @on-need-pipeline
	 */
	signals[SIGNAL_SET_PIPELINE] = g_signal_new_class_handler(
	    "set-pipeline", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
	    G_CALLBACK(em_connection_set_pipeline), NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

	/**
	 * EmConnection::websocket-connected
	 * @object: the #EmConnection
	 */
	signals[SIGNAL_WEBSOCKET_CONNECTED] = g_signal_new("websocket-connected", G_OBJECT_CLASS_TYPE(klass),
	                                                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * EmConnection::websocket-failed
	 * @object: the #EmConnection
	 */
	signals[SIGNAL_WEBSOCKET_FAILED] = g_signal_new("websocket-failed", G_OBJECT_CLASS_TYPE(klass),
	                                                G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	/**
	 * EmConnection::connected
	 * @object: the #EmConnection
	 */
	signals[SIGNAL_CONNECTED] = g_signal_new("connected", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL,
	                                         NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * EmConnection::on-need-pipeline
	 * @object: the #EmConnection
	 *
	 * Your handler for this must emit @set-pipeline
	 */
	signals[SIGNAL_ON_NEED_PIPELINE] = g_signal_new("on-need-pipeline", G_OBJECT_CLASS_TYPE(klass),
	                                                G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * EmConnection::on-drop-pipeline
	 * @object: the #EmConnection
	 *
	 * If you store any references in your handler for @on-need-pipeline you must make a handler for this signal to
	 * drop them.
	 */
	signals[SIGNAL_ON_DROP_PIPELINE] = g_signal_new("on-drop-pipeline", G_OBJECT_CLASS_TYPE(klass),
	                                                G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ALOGE("RYLIE: %s: End", __FUNCTION__);
}


#define MAKE_CASE(E)                                                                                                   \
	case E: return #E

static const char *
peer_connection_state_to_string(GstWebRTCPeerConnectionState state)
{

	switch (state) {
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_NEW);
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING);
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED);
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED);
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_FAILED);
		MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED);
	default: return "!Unknown!";
	}
}
#undef MAKE_CASE

static void
emconn_update_status(EmConnection *emconn, enum em_status status)
{
	if (status == emconn->status) {
		ALOGI("emconn: state update: already in %s", em_status_to_string(emconn->status));
		return;
	}
	ALOGI("emconn: state update: %s -> %s", em_status_to_string(emconn->status), em_status_to_string(status));
	emconn->status = status;
}

static void
emconn_update_status_from_peer_connection_state(EmConnection *emconn, GstWebRTCPeerConnectionState state)
{
	switch (state) {
	case GST_WEBRTC_PEER_CONNECTION_STATE_NEW: break;
	case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING: emconn_update_status(emconn, EM_STATUS_NEGOTIATING); break;
	case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
		emconn_update_status(emconn, EM_STATUS_CONNECTED_NO_DATA);
		break;

	case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
	case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED: emconn_update_status(emconn, EM_STATUS_IDLE_NOT_CONNECTED); break;

	case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED: emconn_update_status(emconn, EM_STATUS_DISCONNECTED_ERROR); break;
	}
}

static void
emconn_disconnect_internal(EmConnection *emconn, enum em_status status)
{
	if (emconn->ws_cancel != NULL) {
		g_cancellable_cancel(emconn->ws_cancel);
		gst_clear_object(&emconn->ws_cancel);
	}
	// Stop the pipeline, if it exists
	if (emconn->pipeline != NULL) {
		gst_element_set_state(GST_ELEMENT(emconn->pipeline), GST_STATE_NULL);
		g_signal_emit(emconn, signals[SIGNAL_ON_DROP_PIPELINE], 0);
	}
	if (emconn->ws) {
		soup_websocket_connection_close(emconn->ws, 0, "");
	}
	g_clear_object(&emconn->ws);

	gst_clear_object(&emconn->webrtcbin);
	gst_clear_object(&emconn->datachannel);
	gst_clear_object(&emconn->pipeline);
	emconn_update_status(emconn, status);
}


static void
emconn_data_channel_error_cb(GstWebRTCDataChannel *datachannel, EmConnection *emconn)
{
	ALOGE("RYLIE: %s: error", __FUNCTION__);
	emconn_disconnect_internal(emconn, EM_STATUS_DISCONNECTED_ERROR);
	// abort();
}

static void
emconn_data_channel_close_cb(GstWebRTCDataChannel *datachannel, EmConnection *emconn)
{
	ALOGI("RYLIE: %s: Data channel closed", __FUNCTION__);
	emconn_disconnect_internal(emconn, EM_STATUS_DISCONNECTED_REMOTE_CLOSE);
}

static void
emconn_data_channel_message_string_cb(GstWebRTCDataChannel *datachannel, gchar *str, EmConnection *emconn)
{
	ALOGI("RYLIE: %s: Received data channel message: %s", __FUNCTION__, str);
}

static void
emconn_connect_internal(EmConnection *emconn, enum em_status status);

static void
emconn_webrtc_deep_notify_callback(GstObject *self, GstObject *prop_object, GParamSpec *prop, EmConnection *emconn)
{
	GstWebRTCPeerConnectionState state;
	g_object_get(prop_object, "connection-state", &state, NULL);
	ALOGV("RYLIE: deep-notify callback says peer connection state is %s - but it lies sometimes",
	      peer_connection_state_to_string(state));
	// emconn_update_status_from_peer_connection_state(emconn, state);
}

static void
emconn_webrtc_prepare_data_channel_cb(GstElement *webrtc,
                                      GObject *data_channel,
                                      gboolean is_local,
                                      EmConnection *emconn)
{
	ALOGI("preparing data channel");

	g_signal_connect(data_channel, "on-close", G_CALLBACK(emconn_data_channel_close_cb), emconn);
	g_signal_connect(data_channel, "on-error", G_CALLBACK(emconn_data_channel_error_cb), emconn);
	g_signal_connect(data_channel, "on-message-string", G_CALLBACK(emconn_data_channel_message_string_cb), emconn);
}

static void
emconn_webrtc_on_data_channel_cb(GstElement *webrtcbin, GstWebRTCDataChannel *data_channel, EmConnection *emconn)
{

	ALOGI("Successfully created datachannel");

	g_assert_null(emconn->datachannel);

	emconn->datachannel = GST_WEBRTC_DATA_CHANNEL(data_channel);

	emconn_update_status(emconn, EM_STATUS_CONNECTED);
	g_signal_emit(emconn, signals[SIGNAL_CONNECTED], 0);
}

void
emconn_send_sdp_answer(EmConnection *emconn, const gchar *sdp)
{
	JsonBuilder *builder;
	JsonNode *root;
	gchar *msg_str;

	ALOGI("Send answer: %s", sdp);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "msg");
	json_builder_add_string_value(builder, "answer");

	json_builder_set_member_name(builder, "sdp");
	json_builder_add_string_value(builder, sdp);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);

	msg_str = json_to_string(root, TRUE);
	soup_websocket_connection_send_text(emconn->ws, msg_str);
	g_clear_pointer(&msg_str, g_free);

	json_node_unref(root);
	g_object_unref(builder);
}

static void
emconn_webrtc_on_ice_candidate_cb(GstElement *webrtcbin, guint mlineindex, gchar *candidate, EmConnection *emconn)
{
	JsonBuilder *builder;
	JsonNode *root;
	gchar *msg_str;

	ALOGI("Send candidate: line %u: %s", mlineindex, candidate);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "msg");
	json_builder_add_string_value(builder, "candidate");

	json_builder_set_member_name(builder, "candidate");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "candidate");
	json_builder_add_string_value(builder, candidate);
	json_builder_set_member_name(builder, "sdpMLineIndex");
	json_builder_add_int_value(builder, mlineindex);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);

	msg_str = json_to_string(root, TRUE);
	ALOGD("%s: candidate message: %s", __FUNCTION__, msg_str);
	soup_websocket_connection_send_text(emconn->ws, msg_str);
	g_clear_pointer(&msg_str, g_free);

	json_node_unref(root);
	g_object_unref(builder);
}

static void
emconn_webrtc_on_answer_created(GstPromise *promise, EmConnection *emconn)
{
	GstWebRTCSessionDescription *answer = NULL;
	gchar *sdp;

	ALOGD("%s", __FUNCTION__);
	gst_structure_get(gst_promise_get_reply(promise), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
	gst_promise_unref(promise);

	if (NULL == answer) {
		ALOGE("%s : ERROR !  get_promise answer = null !", __FUNCTION__);
	}

	g_signal_emit_by_name(emconn->webrtcbin, "set-local-description", answer, NULL);

	sdp = gst_sdp_message_as_text(answer->sdp);
	if (NULL == sdp) {
		ALOGE("%s : ERROR !  sdp = null !", __FUNCTION__);
	}
	emconn_send_sdp_answer(emconn, sdp);
	g_free(sdp);

	gst_webrtc_session_description_free(answer);
}

static void
emconn_webrtc_process_sdp_offer(EmConnection *emconn, const gchar *sdp)
{
	GstSDPMessage *sdp_msg = NULL;
	GstWebRTCSessionDescription *desc = NULL;


	ALOGI("Received offer: %s\n", sdp);

	if (gst_sdp_message_new_from_text(sdp, &sdp_msg) != GST_SDP_OK) {
		g_debug("Error parsing SDP description");
		goto out;
	}

	desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_msg);
	if (desc) {
		GstPromise *promise;

		promise = gst_promise_new();

		g_signal_emit_by_name(emconn->webrtcbin, "set-remote-description", desc, promise);

		gst_promise_wait(promise);
		gst_promise_unref(promise);

		g_signal_emit_by_name(emconn->webrtcbin, "create-answer", NULL,
		                      gst_promise_new_with_change_func(
		                          (GstPromiseChangeFunc)emconn_webrtc_on_answer_created, emconn, NULL));
	} else {
		gst_sdp_message_free(sdp_msg);
	}

out:
	g_clear_pointer(&desc, gst_webrtc_session_description_free);
}

static void
emconn_webrtc_process_candidate(EmConnection *emconn, guint mlineindex, const gchar *candidate)
{
	ALOGI("process_candidate: %d %s", mlineindex, candidate);

	g_signal_emit_by_name(emconn->webrtcbin, "add-ice-candidate", mlineindex, candidate);
}

static void
emconn_on_ws_message_cb(SoupWebsocketConnection *connection, gint type, GBytes *message, EmConnection *emconn)
{
	ALOGD("%s", __FUNCTION__);
	gsize length = 0;
	const gchar *msg_data = g_bytes_get_data(message, &length);
	JsonParser *parser = json_parser_new();
	GError *error = NULL;

	// TODO convert gsize to gssize after range check

	if (json_parser_load_from_data(parser, msg_data, length, &error)) {
		JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
		const gchar *msg_type;

		if (!json_object_has_member(msg, "msg")) {
			// Invalid message
			goto out;
		}

		msg_type = json_object_get_string_member(msg, "msg");
		ALOGI("Websocket message received: %s", msg_type);

		if (g_str_equal(msg_type, "offer")) {
			const gchar *offer_sdp = json_object_get_string_member(msg, "sdp");
			emconn_webrtc_process_sdp_offer(emconn, offer_sdp);
		} else if (g_str_equal(msg_type, "candidate")) {
			JsonObject *candidate;

			candidate = json_object_get_object_member(msg, "candidate");

			emconn_webrtc_process_candidate(emconn, json_object_get_int_member(candidate, "sdpMLineIndex"),
			                                json_object_get_string_member(candidate, "candidate"));
		}
	} else {
		g_debug("Error parsing message: %s", error->message);
		g_clear_error(&error);
	}

out:
	g_object_unref(parser);
}


static void
emconn_websocket_connected_cb(GObject *session, GAsyncResult *res, EmConnection *emconn)
{

	GError *error = NULL;

	g_assert(!emconn->ws);

	emconn->ws = g_object_ref_sink(soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error));

	if (error) {
		ALOGW("Websocket connection failed, may not be available.");
		g_signal_emit(emconn, signals[SIGNAL_WEBSOCKET_FAILED], 0);
		emconn_update_status(emconn, EM_STATUS_WEBSOCKET_FAILED);
		return;
	}
	g_assert_no_error(error);
	GstBus *bus;

	ALOGI("RYLIE: Websocket connected");
	g_signal_connect(emconn->ws, "message", G_CALLBACK(emconn_on_ws_message_cb), emconn);
	g_signal_emit(emconn, signals[SIGNAL_WEBSOCKET_CONNECTED], 0);

	ALOGI("RYLIE: creating pipeline");
	g_assert_null(emconn->pipeline);
	g_signal_emit(emconn, signals[SIGNAL_ON_NEED_PIPELINE], 0);
	if (emconn->pipeline == NULL) {
		ALOGE("on-need-pipeline signal did not return a pipeline!");
		em_connection_disconnect(emconn);
		return;
	}
	// OK, if we get here, we have a websocket connection, and a pipeline fully configured
	// so we can start the pipeline playing

	ALOGI("RYLIE: Setting pipeline state to PLAYING");
	gst_element_set_state(GST_ELEMENT(emconn->pipeline), GST_STATE_PLAYING);
	ALOGI("%s: RYLIE: Done with function", __FUNCTION__);
}


void
em_connection_set_pipeline(EmConnection *emconn, GstPipeline *pipeline)
{
	g_assert_nonnull(pipeline);
	if (emconn->pipeline) {
		// stop old pipeline if applicable
		gst_element_set_state(GST_ELEMENT(emconn->pipeline), GST_STATE_NULL);
	}
	gst_clear_object(&emconn->pipeline);
	emconn->pipeline = gst_object_ref_sink(pipeline);

	emconn_update_status(emconn, EM_STATUS_NEGOTIATING);

	ALOGI("RYLIE: getting webrtcbin");
	emconn->webrtcbin = gst_bin_get_by_name(GST_BIN(emconn->pipeline), "webrtc");
	g_assert_nonnull(emconn->webrtcbin);
	g_assert(G_IS_OBJECT(emconn->webrtcbin));
	g_signal_connect(emconn->webrtcbin, "on-ice-candidate", G_CALLBACK(emconn_webrtc_on_ice_candidate_cb), emconn);
	g_signal_connect(emconn->webrtcbin, "prepare-data-channel", G_CALLBACK(emconn_webrtc_prepare_data_channel_cb),
	                 emconn);
	g_signal_connect(emconn->webrtcbin, "on-data-channel", G_CALLBACK(emconn_webrtc_on_data_channel_cb), emconn);
	g_signal_connect(emconn->webrtcbin, "deep-notify::connection-state",
	                 G_CALLBACK(emconn_webrtc_deep_notify_callback), emconn);
}

static void
emconn_connect_internal(EmConnection *emconn, enum em_status status)
{
	em_connection_disconnect(emconn);
	if (!emconn->ws_cancel) {
		emconn->ws_cancel = g_cancellable_new();
	}
	g_cancellable_reset(emconn->ws_cancel);
	ALOGI("RYLIE: calling soup_session_websocket_connect_async. websocket_uri = %s", emconn->websocket_uri);
#if SOUP_MAJOR_VERSION == 2
	soup_session_websocket_connect_async(emconn->soup_session,                                     // session
	                                     soup_message_new(SOUP_METHOD_GET, emconn->websocket_uri), // message
	                                     NULL,                                                     // origin
	                                     NULL,                                                     // protocols
	                                     emconn->ws_cancel,                                        // cancellable
	                                     (GAsyncReadyCallback)emconn_websocket_connected_cb,       // callback
	                                     emconn);                                                  // user_data

#else
	soup_session_websocket_connect_async(emconn->soup_session,                                     // session
	                                     soup_message_new(SOUP_METHOD_GET, emconn->websocket_uri), // message
	                                     NULL,                                                     // origin
	                                     NULL,                                                     // protocols
	                                     0,                                                        // io_prority
	                                     emconn->ws_cancel,                                        // cancellable
	                                     (GAsyncReadyCallback)emconn_websocket_connected_cb,       // callback
	                                     emconn);                                                  // user_data

#endif
	emconn_update_status(emconn, status);
}


/* public (non-GObject) methods */

EmConnection *
em_connection_new(const gchar *websocket_uri)
{
	return EM_CONNECTION(g_object_new(EM_TYPE_CONNECTION, "websocket-uri", websocket_uri, NULL));
}

EmConnection *
em_connection_new_localhost()
{
	return EM_CONNECTION(g_object_new(EM_TYPE_CONNECTION, NULL));
}

void
em_connection_connect(EmConnection *emconn)
{
	emconn_connect_internal(emconn, EM_STATUS_CONNECTING);
}

void
em_connection_disconnect(EmConnection *emconn)
{
	emconn_disconnect_internal(emconn, EM_STATUS_IDLE_NOT_CONNECTED);
}

bool
em_connection_send_bytes(EmConnection *emconn, GBytes *bytes)
{
	if (emconn->status != EM_STATUS_CONNECTED) {
		ALOGW("RYLIE: Cannot send bytes when status is %s", em_status_to_string(emconn->status));
		return false;
	}

	gboolean success = gst_webrtc_data_channel_send_data_full(emconn->datachannel, bytes, NULL);

	return success == TRUE;
}
