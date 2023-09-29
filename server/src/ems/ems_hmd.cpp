// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief Electric Maple Server HMD device
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @ingroup drv_ems
 */

#include "ems_callbacks.h"
#include "xrt/xrt_defines.h"
#undef CLAMP

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_distortion_mesh.h"



#include "electricmaple.pb.h"
#include "pb_decode.h"

#include "ems_server_internal.h"

#include <cstdint>
#include <glib.h>
#include <memory>
#include <mutex>
#include <stdio.h>



/*
 *
 * Structs and defines.
 *
 */

/*!
 * The remote HMD device.
 *
 * @implements xrt_device
 */

static constexpr uint64_t kFixedAssumedLatencyUs = 50 * U_TIME_1MS_IN_NS;


/// Casting helper function
static inline struct ems_hmd *
ems_hmd(struct xrt_device *xdev)
{
	return (struct ems_hmd *)xdev;
}

DEBUG_GET_ONCE_LOG_OPTION(sample_log, "EMS_LOG", U_LOGGING_WARN)

#define EMS_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define EMS_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define EMS_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

static void
ems_hmd_destroy(struct xrt_device *xdev)
{
	struct ems_hmd *eh = ems_hmd(xdev);

	eh->received = nullptr;

	m_relation_history_destroy(&eh->pose_history);

	// Remove the variable tracking.
	u_var_remove_root(eh);

	u_device_free(&eh->base);
}

static void
ems_hmd_update_inputs(struct xrt_device *xdev)
{
	// Empty, you should put code to update the attached input fields (if any)
}

static void
ems_hmd_get_tracked_pose(struct xrt_device *xdev,
                         enum xrt_input_name name,
                         uint64_t at_timestamp_ns,
                         struct xrt_space_relation *out_relation)
{
	struct ems_hmd *eh = ems_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		EMS_ERROR(eh, "unknown input name");
		return;
	}

	if (eh->received->updated) {
		xrt_space_relation rel;
		uint64_t timestamp;
		std::lock_guard<std::mutex> lock(eh->received->mutex);
		rel = eh->received->rel;
		timestamp = eh->received->timestamp;
		eh->received->rel.relation_flags = XRT_SPACE_RELATION_BITMASK_NONE;
		eh->received->updated = false;
		if (0 == (rel.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)) {
			// guess our velocities if not reported.
			m_relation_history_estimate_motion(eh->pose_history, &rel, timestamp, &rel);
		}
		m_relation_history_push(eh->pose_history, &rel, timestamp);
	}
	m_relation_history_get(eh->pose_history, at_timestamp_ns, out_relation);
}

static void
ems_hmd_get_view_poses(struct xrt_device *xdev,
                       const struct xrt_vec3 *default_eye_relation,
                       uint64_t at_timestamp_ns,
                       uint32_t view_count,
                       struct xrt_space_relation *out_head_relation,
                       struct xrt_fov *out_fovs,
                       struct xrt_pose *out_poses)
{
	u_device_get_view_poses(xdev, default_eye_relation, at_timestamp_ns, view_count, out_head_relation, out_fovs,
	                        out_poses);
}

xrt_vec3
to_xrt_vec3(const em_proto_Vec3 &v)
{
	return {v.x, v.y, v.z};
}

static void
ems_hmd_handle_data(enum ems_callbacks_event event, const em_proto_UpMessage *message, void *userdata)
{
	struct ems_hmd *eh = (struct ems_hmd *)userdata;

	if (!message->has_tracking) {
		return;
	}
	struct xrt_pose pose = {};
	pose.position = to_xrt_vec3(message->tracking.P_localSpace_viewSpace.position);

	pose.orientation.w = message->tracking.P_localSpace_viewSpace.orientation.w;
	pose.orientation.x = message->tracking.P_localSpace_viewSpace.orientation.x;
	pose.orientation.y = message->tracking.P_localSpace_viewSpace.orientation.y;
	pose.orientation.z = message->tracking.P_localSpace_viewSpace.orientation.z;
	uint64_t now = os_monotonic_get_ns();
	// TODO handle timestamp, etc

	xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.pose = pose;
	math_quat_normalize(&rel.pose.orientation);
	rel.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	if (message->tracking.has_V_localSpace_viewSpace_linear) {
		rel.linear_velocity = to_xrt_vec3(message->tracking.V_localSpace_viewSpace_linear);
		rel.relation_flags =
		    (enum xrt_space_relation_flags)(rel.relation_flags | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
	}
	if (message->tracking.has_V_localSpace_viewSpace_angular) {
		rel.angular_velocity = to_xrt_vec3(message->tracking.V_localSpace_viewSpace_angular);
		rel.relation_flags =
		    (enum xrt_space_relation_flags)(rel.relation_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	}
	{
		std::lock_guard<std::mutex> lock(eh->received->mutex);
		eh->received->rel = rel;
		eh->received->timestamp = now - kFixedAssumedLatencyUs;
		eh->received->updated = true;
	}
}

struct ems_hmd *
ems_hmd_create(ems_instance &emsi)
{
	// We only want the HMD parts and one input.
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD);

	struct ems_hmd *eh = U_DEVICE_ALLOCATE(struct ems_hmd, flags, 1, 0);

	eh->received = std::make_unique<ems_hmd_recvbuf>();

	// Functions.
	eh->base.update_inputs = ems_hmd_update_inputs;
	eh->base.get_tracked_pose = ems_hmd_get_tracked_pose;
	eh->base.get_view_poses = ems_hmd_get_view_poses;
	eh->base.destroy = ems_hmd_destroy;

	// Public data.
	eh->base.name = XRT_DEVICE_GENERIC_HMD;
	eh->base.device_type = XRT_DEVICE_TYPE_HMD;
	eh->base.tracking_origin = &emsi.tracking_origin;
	eh->base.orientation_tracking_supported = true;
	eh->base.position_tracking_supported = false;

	// Private data.
	eh->instance = &emsi;
	// eh->pose = (struct xrt_pose){XRT_QUAT_IDENTITY, {0.0f, 1.6f, 0.0f}};
	eh->log_level = debug_get_log_option_sample_log();

	// Print name.
	snprintf(eh->base.str, XRT_DEVICE_NAME_LEN, "Electric Maple Server HMD");
	snprintf(eh->base.serial, XRT_DEVICE_NAME_LEN, "EMS HMD S/N");

	// Setup input.
	eh->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// This list should be ordered, most preferred first.
	size_t idx = 0;
	eh->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	eh->base.hmd->blend_mode_count = idx;

	// TODO: Find out the framerate that the remote device runs at
	eh->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 90.0f);

	// TODO: Find out the remote device's actual FOV. Or maybe remove this because I think get_view_poses lets us
	// set the FOV dynamically.

	eh->base.hmd->distortion.fov[0] = (xrt_fov){
	    // .angle_left = -0.75f,
	    .angle_left = -0.855f,
	    .angle_right = 0.785f,
	    .angle_up = 0.838f,
	    .angle_down = -0.873f,
	};
	eh->base.hmd->distortion.fov[1] = (xrt_fov){
	    .angle_left = -0.785f,
	    // .angle_right = 0.75f,
	    .angle_right = 0.855f,
	    .angle_up = 0.838f,
	    .angle_down = -0.873f,
	};

	// TODO: Ditto, figure out the device's actual resolution
	const int panel_w = 1080;
	const int panel_h = 1200;

	// Single "screen" (always the case)
	eh->base.hmd->screens[0].w_pixels = panel_w * 2;
	eh->base.hmd->screens[0].h_pixels = panel_h;

	// Left, Right
	for (uint8_t eye = 0; eye < 2; ++eye) {
		eh->base.hmd->views[eye].display.w_pixels = panel_w;
		eh->base.hmd->views[eye].display.h_pixels = panel_h;
		eh->base.hmd->views[eye].viewport.y_pixels = 0;
		eh->base.hmd->views[eye].viewport.w_pixels = panel_w;
		eh->base.hmd->views[eye].viewport.h_pixels = panel_h;
		// if rotation is not identity, the dimensions can get more complex.
		eh->base.hmd->views[eye].rot = u_device_rotation_ident;
	}
	// left eye starts at x=0, right eye starts at x=panel_width
	eh->base.hmd->views[0].viewport.x_pixels = 0;
	eh->base.hmd->views[1].viewport.x_pixels = panel_w;

	ems_callbacks_add(emsi.callbacks, EMS_CALLBACKS_EVENT_TRACKING, ems_hmd_handle_data, eh);

	// TODO: Doing anything with distortion here makes no sense
	u_distortion_mesh_set_none(&eh->base);

	// TODO: Are we going to have any actual useful info to show here?
	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(eh, "Electric Maple Server HMD", true);
	u_var_add_log_level(eh, &eh->log_level, "log_level");

	return eh;
}
