add_library(ems_gst STATIC ems_gstreamer_pipeline.c ems_signaling_server.c)

target_link_libraries(
	ems_gst
	PRIVATE
		ems_build_defines
		ems_callbacks
		em_proto
		aux_util
		aux_gstreamer
		${GST_LIBRARIES}
		${GST_SDP_LIBRARIES}
		${GST_WEBRTC_LIBRARIES}
		${GLIB_LIBRARIES}
		${LIBSOUP_LIBRARIES}
		${JSONGLIB_LIBRARIES}
		${GIO_LIBRARIES}
	)

target_include_directories(
	ems_gst
	PRIVATE
		${GLIB_INCLUDE_DIRS}
		${GST_INCLUDE_DIRS}
		${LIBSOUP_INCLUDE_DIRS}
		${JSONGLIB_INCLUDE_DIRS}
		${GIO_INCLUDE_DIRS}
	)
