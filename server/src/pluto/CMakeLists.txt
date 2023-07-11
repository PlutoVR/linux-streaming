# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_subdirectory(include)
add_subdirectory(gst)

add_library(comp_pluto STATIC pl_comp.cpp pl_comp.h)
target_link_libraries(
	comp_pluto
	PUBLIC xrt-interfaces
	PRIVATE
		aux_util
		aux_os
		aux_vk
		comp_util
		comp_multi
		pl_gst
	)
target_include_directories(comp_pluto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(comp_pluto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(drv_pluto STATIC pl_driver.cpp pl_device_controller.cpp)

target_link_libraries(drv_pluto PRIVATE xrt-interfaces aux_util pluto_proto)

add_executable(
	pluto_streaming_server pl_instance.cpp pl_server_internal.h pl_server_main.cpp pl_comms.cpp
	)

target_link_libraries(
	pluto_streaming_server
	PRIVATE
		aux_util
		aux_util_debug_gui
		st_prober
		ipc_server
		comp_main
		drv_pluto
		pluto_proto
		comp_pluto
	)

target_link_libraries(pluto_streaming_server PRIVATE st_gui xrt-external-imgui-sdl2 aux_ogl)
