// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Header
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @ingroup xrt_fs_em
 */

#pragma once

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <EGL/egl.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <string.h>
#include <stdbool.h>

struct em_sample
{
	GLuint frame_texture_id;
	GLenum frame_texture_target;

	bool have_poses;
	XrPosef poses[2];
};
