# Copyright 2019-2023, Collabora, Ltd.
# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

add_subdirectory(include)

add_library(ems_callbacks STATIC ems_callbacks.cpp)
target_link_libraries(
	ems_callbacks
	PUBLIC xrt-interfaces
	PRIVATE aux_util
	)

target_include_directories(ems_callbacks PUBLIC . ${GLIB_INCLUDE_DIRS})

add_subdirectory(gst)

add_library(comp_ems STATIC ems_compositor.cpp ems_compositor.h)
target_link_libraries(
	comp_ems
	PUBLIC xrt-interfaces
	PRIVATE
		aux_util
		aux_os
		aux_vk
		comp_util
		comp_multi
		ems_gst
		em_proto
	)
target_include_directories(comp_ems PUBLIC . ${GST_INCLUDE_DIRS})

add_library(drv_ems STATIC ems_hmd.cpp ems_motion_controller.cpp)

target_link_libraries(drv_ems PRIVATE xrt-interfaces aux_util em_proto ems_callbacks)

add_executable(ems_streaming_server ems_instance.cpp ems_server_internal.h ems_server_main.cpp)

target_link_libraries(
	ems_streaming_server
	PRIVATE
		aux_util
		aux_util_debug_gui
		st_prober
		ipc_server
		comp_main
		drv_ems
		em_proto
		comp_ems
		ems_callbacks
	)

target_link_libraries(ems_streaming_server PRIVATE st_gui xrt-external-imgui-sdl2 aux_ogl)

install(TARGETS ems_streaming_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
