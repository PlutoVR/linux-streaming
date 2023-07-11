// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief Pluto HMD device
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Ryan Pavlik <ryan.pavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @ingroup drv_sample
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_distortion_mesh.h"



#include "pluto.pb.h"
#include "pb_decode.h"

#include "pl_server_internal.h"

#include "stdio.h"

#include <thread>



/*
 *
 * Structs and defines.
 *
 */

/*!
 * A sample HMD device.
 *
 * @implements xrt_device
 */



/// Casting helper function
static inline struct pluto_hmd *
pluto_hmd(struct xrt_device *xdev)
{
	return (struct pluto_hmd *)xdev;
}

DEBUG_GET_ONCE_LOG_OPTION(sample_log, "PLUTO_LOG", U_LOGGING_WARN)

#define PL_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define PL_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define PL_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

static void
pluto_hmd_destroy(struct xrt_device *xdev)
{
	struct pluto_hmd *ph = pluto_hmd(xdev);

	// Remove the variable tracking.
	u_var_remove_root(ph);

	u_device_free(&ph->base);
}

static void
pluto_hmd_update_inputs(struct xrt_device *xdev)
{
	// Empty, you should put code to update the attached input fields (if any)
}

static void
pluto_hmd_get_tracked_pose(struct xrt_device *xdev,
                           enum xrt_input_name name,
                           uint64_t at_timestamp_ns,
                           struct xrt_space_relation *out_relation)
{
	struct pluto_hmd *ph = pluto_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		PL_ERROR(ph, "unknown input name");
		return;
	}

	// Estimate pose at timestamp at_timestamp_ns!
	math_quat_normalize(&ph->pose.orientation);
	out_relation->pose = ph->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
}

static void
pluto_hmd_get_view_poses(struct xrt_device *xdev,
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



// extern "C"
struct pluto_hmd *
pluto_hmd_create(pluto_program &pp)
{
	// We only want the HMD parts and one input.
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD);

	struct pluto_hmd *ph = U_DEVICE_ALLOCATE(struct pluto_hmd, flags, 1, 0);

	// Functions.
	ph->base.update_inputs = pluto_hmd_update_inputs;
	ph->base.get_tracked_pose = pluto_hmd_get_tracked_pose;
	ph->base.get_view_poses = pluto_hmd_get_view_poses;
	ph->base.destroy = pluto_hmd_destroy;

	// Public data.
	ph->base.name = XRT_DEVICE_GENERIC_HMD;
	ph->base.device_type = XRT_DEVICE_TYPE_HMD;
	ph->base.tracking_origin = &pp.tracking_origin;
	ph->base.orientation_tracking_supported = true;
	ph->base.position_tracking_supported = false;

	// Private data.
	ph->program = &pp;
	ph->pose = (struct xrt_pose){XRT_QUAT_IDENTITY, {0.0f, 1.6f, 0.0f}};
	ph->log_level = debug_get_log_option_sample_log();

	// Print name.
	snprintf(ph->base.str, XRT_DEVICE_NAME_LEN, "Pluto HMD");
	snprintf(ph->base.serial, XRT_DEVICE_NAME_LEN, "Pluto HMD S/N");

	// Setup input.
	ph->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// This list should be ordered, most preferred first.
	size_t idx = 0;
	ph->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	ph->base.hmd->blend_mode_count = idx;

	// TODO: Find out the framerate that the remote device runs at
	ph->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 90.0f);

	// TODO: Find out the remote device's actual FOV. Or maybe remove this because I think get_view_poses lets us
	// set the FOV dynamically.
	const double hFOV = 90 * (M_PI / 180.0);
	const double vFOV = 96.73 * (M_PI / 180.0);
	// center of projection
	const double hCOP = 0.529;
	const double vCOP = 0.5;
	if (
	    /* right eye */
	    !math_compute_fovs(1, hCOP, hFOV, 1, vCOP, vFOV, &ph->base.hmd->distortion.fov[1]) ||
	    /*
	     * left eye - same as right eye, except the horizontal center of projection is moved in the opposite
	     * direction now
	     */
	    !math_compute_fovs(1, 1.0 - hCOP, hFOV, 1, vCOP, vFOV, &ph->base.hmd->distortion.fov[0])) {
		// If those failed, it means our math was impossible.
		PL_ERROR(ph, "Failed to setup basic device info");
		pluto_hmd_destroy(&ph->base);
		return NULL;
	}
	// TODO: Ditto, figure out the device's actual resolution
	const int panel_w = 1080;
	const int panel_h = 1200;

	// Single "screen" (always the case)
	ph->base.hmd->screens[0].w_pixels = panel_w * 2;
	ph->base.hmd->screens[0].h_pixels = panel_h;

	// Left, Right
	for (uint8_t eye = 0; eye < 2; ++eye) {
		ph->base.hmd->views[eye].display.w_pixels = panel_w;
		ph->base.hmd->views[eye].display.h_pixels = panel_h;
		ph->base.hmd->views[eye].viewport.y_pixels = 0;
		ph->base.hmd->views[eye].viewport.w_pixels = panel_w;
		ph->base.hmd->views[eye].viewport.h_pixels = panel_h;
		// if rotation is not identity, the dimensions can get more complex.
		ph->base.hmd->views[eye].rot = u_device_rotation_ident;
	}
	// left eye starts at x=0, right eye starts at x=panel_width
	ph->base.hmd->views[0].viewport.x_pixels = 0;
	ph->base.hmd->views[1].viewport.x_pixels = panel_w;

	// TODO: Doing anything wiht distortion here makes no sense
	u_distortion_mesh_set_none(&ph->base);

	// TODO: Are we going to have any actual useful info to show here?
	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(ph, "Pluto HMD", true);
	u_var_add_pose(ph, &ph->pose, "pose");
	u_var_add_log_level(ph, &ph->log_level, "log_level");

	// make_connect_socket(*ph);

	// ph->aaaaa = std::thread(run_comms_thread, ph);


	return ph;
}
