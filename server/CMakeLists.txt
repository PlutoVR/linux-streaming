# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.10.2)
project(linux-streaming VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

# Make sure we have pretty colours
option(DISABLE_COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." OFF)

if(NOT DISABLE_COLORED_OUTPUT)
	if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
		add_compile_options(-fdiagnostics-color=always)
	elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
		add_compile_options(-fcolor-diagnostics)
	endif()
endif()

# Get Monado's CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/monado/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/monado/cmake/sanitizers")

if(NOT MSVC)
	set(CMAKE_C_FLAGS
	    "${CMAKE_C_FLAGS} -pedantic -Wall -Wextra -Wno-unused-parameter -Werror=incompatible-pointer-types"
		)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-undefined")
endif()

option(EMS_LIBSOUP2 "Use libsoup2.4 instead of libsoup3.0" OFF)

# pkgconfig!
find_package(PkgConfig REQUIRED)

# GLib packages we'll need
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GST_SDP REQUIRED gstreamer-sdp-1.0)
pkg_check_modules(GST_RTP REQUIRED gstreamer-rtp-1.0)
pkg_check_modules(GST_WEBRTC REQUIRED gstreamer-webrtc-1.0)
pkg_check_modules(GST REQUIRED gstreamer-plugins-base-1.0)
pkg_check_modules(GST REQUIRED gstreamer-plugins-bad-1.0)

if(EMS_LIBSOUP2)
	pkg_check_modules(LIBSOUP REQUIRED libsoup-2.4)
else()
	pkg_check_modules(LIBSOUP REQUIRED libsoup-3.0)
endif()

pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
pkg_check_modules(GIO REQUIRED gio-2.0)

# Default to PIC code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MONADO_SRC_XRT ${CMAKE_CURRENT_SOURCE_DIR}/monado/src/xrt)

### Set configuration options for Monado

# Helper to obey CMP0077 and work around warnings.
function(setForce NAME VALUE)
	set(${NAME}
	    ${VALUE}
	    CACHE INTERNAL "" FORCE
		)
endfunction()

# Let's use this for now: Monado's main compositor is really good and is an
#easy way to see that things are working.
setforce(XRT_FEATURE_COMPOSITOR_MAIN ON)

# No reason to disable this
setforce(XRT_FEATURE_COMPOSITOR_NULL ON)

# It looks like we can use Monado's normal out-of-proc OpenXR instance as-is. We'll see.
setforce(XRT_FEATURE_OPENXR ON)

setforce(XRT_FEATURE_RENDERDOC OFF) # we are not using this
setforce(XRT_FEATURE_STEAMVR_PLUGIN OFF) # we are not using this
setforce(XRT_FEATURE_TRACING OFF) # we are not using this yet
setforce(XRT_FEATURE_SLAM OFF) # we are not using this

setforce(XRT_FEATURE_CLIENT_DEBUG_GUI OFF) # we are not using this

setforce(XRT_MODULE_MONADO_CLI OFF) # we are not using this
setforce(XRT_MODULE_MONADO_GUI OFF) # we are not using this
setforce(XRT_MODULE_MERCURY_HANDTRACKING OFF)

setforce(XRT_HAVE_D3D11 OFF)
setforce(XRT_HAVE_D3D12 OFF)
setforce(XRT_HAVE_OPENCV OFF)

# No drivers!
setforce(XRT_BUILD_DRIVER_ANDROID OFF)
setforce(XRT_BUILD_DRIVER_ARDUINO OFF)
setforce(XRT_BUILD_DRIVER_DAYDREAM OFF)
setforce(XRT_BUILD_DRIVER_DEPTHAI OFF)
setforce(XRT_BUILD_DRIVER_EUROC OFF)
setforce(XRT_BUILD_DRIVER_HANDTRACKING OFF)
setforce(XRT_BUILD_DRIVER_TWRAP OFF)
setforce(XRT_BUILD_DRIVER_HDK OFF)
setforce(XRT_BUILD_DRIVER_HYDRA OFF)
setforce(XRT_BUILD_DRIVER_ILLIXR OFF)
setforce(XRT_BUILD_DRIVER_NS OFF)
setforce(XRT_BUILD_DRIVER_OHMD OFF)
setforce(XRT_BUILD_DRIVER_OPENGLOVES OFF)
setforce(XRT_BUILD_DRIVER_PSMV OFF)
setforce(XRT_BUILD_DRIVER_PSVR OFF)
setforce(XRT_BUILD_DRIVER_QWERTY OFF)
setforce(XRT_BUILD_DRIVER_REALSENSE OFF)
setforce(XRT_BUILD_DRIVER_REMOTE OFF)
setforce(XRT_BUILD_DRIVER_RIFT_S OFF)
setforce(XRT_BUILD_DRIVER_SURVIVE OFF)
setforce(XRT_BUILD_DRIVER_ULV2 OFF)
setforce(XRT_BUILD_DRIVER_VF ON)
setforce(XRT_BUILD_DRIVER_VIVE OFF)
setforce(XRT_BUILD_DRIVER_WMR OFF)
setforce(XRT_BUILD_DRIVER_SIMULAVR OFF)
setforce(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE OFF)

# There is no clean way to disable the prober/builders (as far as I can see?)
# So we enable one driver so that the legacy builder does not die
setforce(XRT_BUILD_DRIVER_SIMULATED ON)

setforce(XRT_BUILD_SAMPLES OFF)

setforce(BUILD_TESTING OFF)
setforce(BUILD_DOC OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(XRT_OXR_RUNTIME_SUFFIX electricmaple CACHE STRING "OpenXR client library suffix" FORCE)

add_subdirectory(../monado ${CMAKE_CURRENT_BINARY_DIR}/monado)

add_subdirectory(../proto ${CMAKE_CURRENT_BINARY_DIR}/proto)

add_subdirectory(src)
