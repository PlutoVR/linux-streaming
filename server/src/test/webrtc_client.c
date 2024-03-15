#include <glib-unix.h>
#include <gst/gst.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup-message.h>
#include <libsoup/soup-session.h>

#include <json-glib/json-glib.h>
#include "stdio.h"
#include "util/u_logging.h"

static gchar *websocket_uri = NULL;

static GOptionEntry options[] = {{
                                     "websocket-uri",
                                     'u',
                                     0,
                                     G_OPTION_ARG_STRING,
                                     &websocket_uri,
                                     "Websocket URI of webrtc signaling connection",
                                     "URI",
                                 },
                                 {NULL}};

#define WEBSOCKET_URI_DEFAULT "ws://127.0.0.1:8080/ws"

//!@todo Don't use global state
static SoupWebsocketConnection *ws = NULL;
static GstElement *pipeline = NULL;
static GstElement *webrtcbin = NULL;
static GstWebRTCDataChannel *datachannel = NULL;


/*
 *
 * Data channel functions.
 *
 */

static void
data_channel_error_cb(GstWebRTCDataChannel *datachannel, void *data)
{
	U_LOG_E("Error");
	abort();
}

static void
data_channel_close_cb(GstWebRTCDataChannel *datachannel, gpointer timeout_src_id)
{
	U_LOG_I("Data channel closed");

	g_source_remove(GPOINTER_TO_UINT(timeout_src_id));
	g_clear_object(&datachannel);
}

static void
data_channel_message_data_cb(GstWebRTCDataChannel *datachannel, GBytes *data, void *user_data)
{
	U_LOG_I("Received data channel message data: %u", (uint32_t)g_bytes_get_size(data));
}

static void
data_channel_message_string_cb(GstWebRTCDataChannel *datachannel, gchar *str, void *user_data)
{
	U_LOG_I("Received data channel message string: %s", str);
}

static gboolean
datachannel_send_message(gpointer unused)
{
	g_signal_emit_by_name(datachannel, "send-string", "Hi! from EMS test client");

	return G_SOURCE_CONTINUE;
}

static void
webrtc_on_data_channel_cb(GstElement *webrtcbin, GstWebRTCDataChannel *data_channel, void *user_data)
{
	guint timeout_src_id;

	U_LOG_I("Successfully created datachannel");

	g_assert_null(datachannel);

	datachannel = GST_WEBRTC_DATA_CHANNEL(data_channel);

	timeout_src_id = g_timeout_add_seconds(3, datachannel_send_message, NULL);

	g_signal_connect(datachannel, "on-close", G_CALLBACK(data_channel_close_cb), GUINT_TO_POINTER(timeout_src_id));
	g_signal_connect(datachannel, "on-error", G_CALLBACK(data_channel_error_cb), GUINT_TO_POINTER(timeout_src_id));
	g_signal_connect(datachannel, "on-message-data", G_CALLBACK(data_channel_message_data_cb), NULL);
	g_signal_connect(datachannel, "on-message-string", G_CALLBACK(data_channel_message_string_cb), NULL);
}


/*
 *
 * Websocket connection.
 *
 */

static gboolean
sigint_handler(gpointer user_data)
{
	g_main_loop_quit(user_data);
	return G_SOURCE_REMOVE;
}

static gboolean
gst_bus_cb(GstBus *bus, GstMessage *message, gpointer data)
{
	GstBin *pipeline = GST_BIN(data);

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

void
send_sdp_answer(const gchar *sdp)
{
	JsonBuilder *builder;
	JsonNode *root;
	gchar *msg_str;

	g_print("Send answer: %s\n", sdp);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "msg");
	json_builder_add_string_value(builder, "answer");

	json_builder_set_member_name(builder, "sdp");
	json_builder_add_string_value(builder, sdp);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);

	msg_str = json_to_string(root, TRUE);
	soup_websocket_connection_send_text(ws, msg_str);
	g_clear_pointer(&msg_str, g_free);

	json_node_unref(root);
	g_object_unref(builder);
}

static void
webrtc_on_ice_candidate_cb(GstElement *webrtcbin, guint mlineindex, gchar *candidate)
{
	JsonBuilder *builder;
	JsonNode *root;
	gchar *msg_str;

	g_print("Send candidate: %u %s\n", mlineindex, candidate);

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
	soup_websocket_connection_send_text(ws, msg_str);
	g_clear_pointer(&msg_str, g_free);

	json_node_unref(root);
	g_object_unref(builder);
}

static void
on_answer_created(GstPromise *promise, gpointer user_data)
{
	GstWebRTCSessionDescription *answer = NULL;
	gchar *sdp;

	gst_structure_get(gst_promise_get_reply(promise), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
	gst_promise_unref(promise);

	g_signal_emit_by_name(webrtcbin, "set-local-description", answer, NULL);

	sdp = gst_sdp_message_as_text(answer->sdp);
	send_sdp_answer(sdp);
	g_free(sdp);

	gst_webrtc_session_description_free(answer);
}

static void
process_sdp_offer(const gchar *sdp)
{
	GstSDPMessage *sdp_msg = NULL;
	GstWebRTCSessionDescription *desc = NULL;

	g_print("Received offer: %s\n", sdp);

	if (gst_sdp_message_new_from_text(sdp, &sdp_msg) != GST_SDP_OK) {
		g_debug("Error parsing SDP description");
		goto out;
	}

	desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_msg);
	if (desc) {
		GstPromise *promise;

		promise = gst_promise_new();

		g_signal_emit_by_name(webrtcbin, "set-remote-description", desc, promise);

		gst_promise_wait(promise);
		gst_promise_unref(promise);

		g_signal_emit_by_name(
		    webrtcbin, "create-answer", NULL,
		    gst_promise_new_with_change_func((GstPromiseChangeFunc)on_answer_created, NULL, NULL));
	} else {
		gst_sdp_message_free(sdp_msg);
	}

out:
	g_clear_pointer(&desc, gst_webrtc_session_description_free);
}

static void
process_candidate(guint mlineindex, const gchar *candidate)
{
	g_print("Received candidate: %d %s\n", mlineindex, candidate);

	g_signal_emit_by_name(webrtcbin, "add-ice-candidate", mlineindex, candidate);
}

static void
message_cb(SoupWebsocketConnection *connection, gint type, GBytes *message, gpointer user_data)
{
	gsize length = 0;
	const gchar *msg_data = g_bytes_get_data(message, &length);
	JsonParser *parser = json_parser_new();
	GError *error = NULL;

	if (json_parser_load_from_data(parser, msg_data, length, &error)) {
		JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
		const gchar *msg_type;

		if (!json_object_has_member(msg, "msg")) {
			// Invalid message
			goto out;
		}

		msg_type = json_object_get_string_member(msg, "msg");
		g_print("Websocket message received: %s\n", msg_type);

		if (g_str_equal(msg_type, "offer")) {
			const gchar *offer_sdp = json_object_get_string_member(msg, "sdp");
			process_sdp_offer(offer_sdp);
		} else if (g_str_equal(msg_type, "candidate")) {
			JsonObject *candidate;

			candidate = json_object_get_object_member(msg, "candidate");

			process_candidate(json_object_get_int_member(candidate, "sdpMLineIndex"),
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
websocket_connected_cb(GObject *session, GAsyncResult *res, gpointer user_data)
{
	GError *error = NULL;

	g_assert(!ws);

	ws = soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
	if (error) {
		g_print("Error creating websocket: %s\n", error->message);
		g_clear_error(&error);
	} else {
		GstBus *bus;

		g_print("Websocket connected\n");
		g_signal_connect(ws, "message", G_CALLBACK(message_cb), NULL);

		pipeline = gst_parse_launch(
		    "webrtcbin name=webrtc bundle-policy=max-bundle ! "
		    "rtph264depay ! "
		    "h264parse ! "
		    "avdec_h264 ! "
		    "autovideosink",
		    &error);
		g_assert_no_error(error);

		// Connect callbacks on sinks.
		webrtcbin = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");

		g_signal_connect(webrtcbin, "on-data-channel", G_CALLBACK(webrtc_on_data_channel_cb), NULL);
		g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(webrtc_on_ice_candidate_cb), NULL);

		bus = gst_element_get_bus(pipeline);
		gst_bus_add_watch(bus, gst_bus_cb, pipeline);
		gst_clear_object(&bus);

		g_assert(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
	}
}

int
main(int argc, char *argv[])
{
	GOptionContext *option_context;
	GMainLoop *loop;
	SoupSession *soup_session;
	GError *error = NULL;

	gst_init(&argc, &argv);

	option_context = g_option_context_new(NULL);
	g_option_context_add_main_entries(option_context, options, NULL);

	if (!g_option_context_parse(option_context, &argc, &argv, &error)) {
		g_print("option parsing failed: %s\n", error->message);
		exit(1);
	}

	if (!websocket_uri) {
		websocket_uri = g_strdup(WEBSOCKET_URI_DEFAULT);
	}

	soup_session = soup_session_new();

#if !SOUP_CHECK_VERSION(3, 0, 0)
	soup_session_websocket_connect_async(soup_session,                                     // session
	                                     soup_message_new(SOUP_METHOD_GET, websocket_uri), // message
	                                     NULL,                                             // origin
	                                     NULL,                                             // protocols
	                                     NULL,                                             // cancellable
	                                     websocket_connected_cb,                           // callback
	                                     NULL);                                            // user_data

#else
	soup_session_websocket_connect_async(soup_session,                                     // session
	                                     soup_message_new(SOUP_METHOD_GET, websocket_uri), // message
	                                     NULL,                                             // origin
	                                     NULL,                                             // protocols
	                                     0,                                                // io_prority
	                                     NULL,                                             // cancellable
	                                     websocket_connected_cb,                           // callback
	                                     NULL);                                            // user_data

#endif

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, sigint_handler, loop);

	g_main_loop_run(loop);
	g_main_loop_unref(loop);
	g_clear_pointer(&websocket_uri, g_free);
}
