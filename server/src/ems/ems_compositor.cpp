// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Remote rendering compositor implementation.
 *
 * Based on the null compositor
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup comp_ems
 */

#include "ems_compositor.h"


#include "electricmaple.pb.h"

#include "gstreamer/gst_internal.h"
#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_verify.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"

#include "util/comp_vulkan.h"

#include "multi/comp_multi_interface.h"

#include "vk/vk_image_readback_to_xf_pool.h"
#include "vk/vk_cmd.h"
#include "vk/vk_cmd_pool.h"


#include <stdio.h>
#include <stdarg.h>

// native quest resolution
// #define APP_VIEW_W (1832)
// #define APP_VIEW_H (1920)


#define APP_VIEW_W (1920)
#define APP_VIEW_H (1920)

// TODO making this 1 causes readback failures
// I assume this means there is some kind of buffer creation failing and we aren't handling the error right.
#define READBACK_DIV_FACTOR (2)

#define READBACK_W2 (APP_VIEW_W / READBACK_DIV_FACTOR)
#define READBACK_W (READBACK_W2 * 2)
#define READBACK_H (APP_VIEW_H / READBACK_DIV_FACTOR)


DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)


/*
 *
 * Helper functions.
 *
 */

static struct vk_bundle *
get_vk(struct ems_compositor *c)
{
	return &c->base.vk;
}


/*
 *
 * Vulkan functions.
 *
 */

static const char *instance_extensions_common[] = {
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,     //
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,  //
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, //
};

static const char *required_device_extensions[] = {
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,            //
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           //
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        //
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, //

// Platform version of "external_memory"
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER)
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,

#else
#error "Need port!"
#endif

// Platform version of "external_fence" and "external_semaphore"
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD) // Optional

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, //
    VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME,     //

#else
#error "Need port!"
#endif
};

static const char *optional_device_extensions[] = {
    VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, //
    VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME,   //

// Platform version of "external_fence" and "external_semaphore"
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)      // Optional
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, //
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,     //

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE) // Not optional

#else
#error "Need port!"
#endif

#ifdef VK_KHR_global_priority
    VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME,
#endif
#ifdef VK_KHR_image_format_list
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
#endif
#ifdef VK_KHR_maintenance1
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
#endif
#ifdef VK_KHR_maintenance2
    VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
#endif
#ifdef VK_KHR_timeline_semaphore
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#endif
#ifdef VK_EXT_calibrated_timestamps
    VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
#endif
#ifdef VK_EXT_robustness2
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
#endif
};

static VkResult
select_instances_extensions(struct ems_compositor *c, struct u_string_list *required, struct u_string_list *optional)
{
#ifdef VK_EXT_display_surface_counter
	u_string_list_append(optional, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
#endif

	return VK_SUCCESS;
}

static bool
compositor_init_vulkan(struct ems_compositor *c)
{
	struct vk_bundle *vk = get_vk(c);
	VkResult ret;

	// every backend needs at least the common extensions
	struct u_string_list *required_instance_ext_list =
	    u_string_list_create_from_array(instance_extensions_common, ARRAY_SIZE(instance_extensions_common));

	struct u_string_list *optional_instance_ext_list = u_string_list_create();

	ret = select_instances_extensions(c, required_instance_ext_list, optional_instance_ext_list);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "select_instances_extensions: %s\n\tFailed to select instance extensions.",
		         vk_result_string(ret));
		u_string_list_destroy(&required_instance_ext_list);
		u_string_list_destroy(&optional_instance_ext_list);
		return ret;
	}

	struct u_string_list *required_device_extension_list =
	    u_string_list_create_from_array(required_device_extensions, ARRAY_SIZE(required_device_extensions));

	struct u_string_list *optional_device_extension_list =
	    u_string_list_create_from_array(optional_device_extensions, ARRAY_SIZE(optional_device_extensions));

	struct comp_vulkan_arguments vk_args = {};

	vk_args.get_instance_proc_address = vkGetInstanceProcAddr;
	vk_args.required_instance_version = VK_MAKE_VERSION(1, 0, 0);
	vk_args.required_instance_extensions = required_instance_ext_list;
	vk_args.optional_instance_extensions = optional_instance_ext_list;
	vk_args.required_device_extensions = required_device_extension_list;
	vk_args.optional_device_extensions = optional_device_extension_list;
	vk_args.log_level = c->settings.log_level;
	vk_args.only_compute_queue = false; // Regular GFX
	vk_args.selected_gpu_index = -1;    // Auto
	vk_args.client_gpu_index = -1;      // Auto
	vk_args.timeline_semaphore = true;  // Flag is optional, not a hard requirement.


	struct comp_vulkan_results vk_res = {};
	bool bundle_ret = comp_vulkan_init_bundle(vk, &vk_args, &vk_res);

	u_string_list_destroy(&required_instance_ext_list);
	u_string_list_destroy(&optional_instance_ext_list);
	u_string_list_destroy(&required_device_extension_list);
	u_string_list_destroy(&optional_device_extension_list);

	if (!bundle_ret) {
		return false;
	}

	// clang-format off
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceUUID.data) == XRT_UUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.selected_gpu_deviceUUID.data) == XRT_UUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceUUID.data) == ARRAY_SIZE(c->sys_info.client_vk_deviceUUID.data), "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.selected_gpu_deviceUUID.data) == ARRAY_SIZE(c->sys_info.compositor_vk_deviceUUID.data), "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceLUID.data) == XRT_LUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceLUID.data) == ARRAY_SIZE(c->sys_info.client_d3d_deviceLUID.data), "array size mismatch");
	// clang-format on

	c->sys_info.client_vk_deviceUUID = vk_res.client_gpu_deviceUUID;
	c->sys_info.compositor_vk_deviceUUID = vk_res.selected_gpu_deviceUUID;
	c->sys_info.client_d3d_deviceLUID = vk_res.client_gpu_deviceLUID;
	c->sys_info.client_d3d_deviceLUID_valid = vk_res.client_gpu_deviceLUID_valid;

	// Init command pool.
	constexpr VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	// U_LOG_I("%s", vk_result_string(ret));
	ret = vk_cmd_pool_init(vk, &c->cmd_pool, flags);
	if (ret != VK_SUCCESS) {
		EMS_COMP_ERROR(c, "vk_cmd_pool_init: %s", vk_result_string(ret));
		return false;
	}

	// Init shared swapchain resources.
	xrt_result_t xret = comp_swapchain_shared_init(&c->base.cscs, vk);
	if (xret != XRT_SUCCESS) {
		EMS_COMP_ERROR(c, "comp_swapchain_shared_init: %u", xret);
		return false;
	}

	return true;
}


/*
 *
 * Other init functions.
 *
 */

static bool
compositor_init_pacing(struct ems_compositor *c)
{
	xrt_result_t xret = u_pc_fake_create(c->settings.frame_interval_ns, os_monotonic_get_ns(), &c->upc);
	if (xret != XRT_SUCCESS) {
		EMS_COMP_ERROR(c, "Failed to create fake pacing helper!");
		return false;
	}

	return true;
}

static bool
compositor_init_info(struct ems_compositor *c)
{
	struct xrt_compositor_info *info = &c->base.base.base.info;

	struct comp_vulkan_formats formats = {};
	comp_vulkan_formats_check(get_vk(c), &formats);
	comp_vulkan_formats_copy_to_info(&formats, info);
	comp_vulkan_formats_log(c->settings.log_level, &formats);

	return true;
}

static bool
compositor_init_sys_info(struct ems_compositor *c, struct xrt_device *xdev)
{
	struct xrt_system_compositor_info *sys_info = &c->sys_info;

	// Required by OpenXR spec.
	sys_info->max_layers = 16;

	// UUIDs and LUID already set in vk init.
	(void)sys_info->compositor_vk_deviceUUID;
	(void)sys_info->client_vk_deviceUUID;
	(void)sys_info->client_d3d_deviceLUID;
	(void)sys_info->client_d3d_deviceLUID_valid;

	// clang-format off

	// These seem to control the
	sys_info->views[0].recommended.width_pixels  = APP_VIEW_W;
	sys_info->views[0].recommended.height_pixels = APP_VIEW_H;
	sys_info->views[0].recommended.sample_count  = 1;
	sys_info->views[0].max.width_pixels          = 2048;
	sys_info->views[0].max.height_pixels         = 2048;
	sys_info->views[0].max.sample_count          = 1;

	sys_info->views[1].recommended.width_pixels  = APP_VIEW_W;
	sys_info->views[1].recommended.height_pixels = APP_VIEW_H;
	sys_info->views[1].recommended.sample_count  = 1;
	sys_info->views[1].max.width_pixels          = 2048;
	sys_info->views[1].max.height_pixels         = 2048;
	sys_info->views[1].max.sample_count          = 1;
	// clang-format on

	// Copy the list directly.
	assert(xdev->hmd->blend_mode_count <= XRT_MAX_DEVICE_BLEND_MODES);
	assert(xdev->hmd->blend_mode_count != 0);
	assert(xdev->hmd->blend_mode_count <= ARRAY_SIZE(sys_info->supported_blend_modes));
	for (size_t i = 0; i < xdev->hmd->blend_mode_count; ++i) {
		assert(u_verify_blend_mode_valid(xdev->hmd->blend_modes[i]));
		sys_info->supported_blend_modes[i] = xdev->hmd->blend_modes[i];
	}
	sys_info->supported_blend_mode_count = (uint8_t)xdev->hmd->blend_mode_count;

	// Refresh rates.
	sys_info->num_refresh_rates = 1;
	sys_info->refresh_rates[0] = (float)(1. / time_ns_to_s(c->settings.frame_interval_ns));

	return true;
}

static inline em_proto_Pose
to_proto(const struct xrt_pose &pose)
{
	em_proto_Pose ret = em_proto_Pose_init_default;
	ret.has_position = true;
	ret.position = {pose.position.x, pose.position.y, pose.position.z};
	ret.has_orientation = true;
	ret.orientation = {pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
	return ret;
}

/*
 *
 * Frame handling functions.
 *
 */

void
pack_blit_and_encode(struct ems_compositor *c,
                     const struct xrt_layer_projection_view_data *lvd,
                     const struct xrt_layer_projection_view_data *rvd,
                     struct comp_swapchain *lsc,
                     struct comp_swapchain *rsc)
{
	if (c->offset_ns == 0) {
		uint64_t now = os_monotonic_get_ns();
		c->offset_ns = now;
		c->gstreamer_sink->offset_ns = now;
	}
	VkResult ret;

	struct vk_image_readback_to_xf *wrap = NULL;
	struct vk_bundle *vk = &c->base.vk;

	// Getting frame
	if (!vk_image_readback_to_xf_pool_get_unused_frame(vk, c->pool, &wrap)) {
		EMS_COMP_ERROR(c, "vk_image_readback_to_xf_pool_get_unused_frame: Failed!");
		return;
	}

	// Usefull.
	xrt_frame *frame = &wrap->base_frame;

	const VkCommandBufferUsageFlags flags = 0;
	VkCommandBuffer cmd = {};

	// For submitting commands.
	vk_cmd_pool_lock(&c->cmd_pool);

	ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->cmd_pool, flags, &cmd);
	if (ret != VK_SUCCESS) {
		EMS_COMP_ERROR(c, "vk_cmd_pool_create_and_begin_cmd_buffer_locked: %s", vk_result_string(ret));
		xrt_frame_reference(&frame, NULL);
		return;
	}

	// Blit images side-by-side (does scaling).
	{
		struct vk_cmd_blit_images_side_by_side_info info = {};

		info.src[0].old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		info.src[0].src_access_mask = VK_ACCESS_SHADER_READ_BIT;
		info.src[0].src_stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		info.src[0].rect = lvd->sub.rect;
		info.src[0].fm_image.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.src[0].fm_image.base_array_layer = lvd->sub.array_index;
		info.src[0].fm_image.image = lsc->vkic.images[lvd->sub.image_index].handle;

		info.src[1].old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		info.src[1].src_access_mask = VK_ACCESS_SHADER_READ_BIT;
		info.src[1].src_stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		info.src[1].rect = rvd->sub.rect;
		info.src[1].fm_image.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.src[1].fm_image.base_array_layer = rvd->sub.array_index;
		info.src[1].fm_image.image = rsc->vkic.images[rvd->sub.image_index].handle;

		info.dst.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.dst.src_access_mask = VK_ACCESS_TRANSFER_READ_BIT;
		info.dst.src_stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		info.dst.size = (xrt_size){READBACK_W, READBACK_H};
		info.dst.fm_image.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.dst.fm_image.base_array_layer = 0;
		info.dst.fm_image.image = c->bounce.image;

		vk_cmd_blit_images_side_by_side_locked(vk, cmd, &info);
	}

	// Copy bounce to destination.
	{
		struct vk_cmd_copy_image_info info = {};

		info.src.old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		info.src.src_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
		info.src.src_stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		info.src.fm_image.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.src.fm_image.base_array_layer = 0;
		info.src.fm_image.image = c->bounce.image;

		info.dst.old_layout = wrap->layout;
		info.dst.src_access_mask = VK_ACCESS_HOST_READ_BIT;
		info.dst.src_stage_mask = VK_PIPELINE_STAGE_HOST_BIT;
		info.dst.fm_image.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.dst.fm_image.base_array_layer = 0;
		info.dst.fm_image.image = wrap->image;

		info.size = (xrt_size){READBACK_W, READBACK_H};

		vk_cmd_copy_image_locked(vk, cmd, &info);
	}

	// Barrier images back, or make ready for read.
	{
		// Copy views into bounce.
		for (int view = 0; view < 2; view++) {

			const xrt_layer_projection_view_data *data = (view == 0) ? lvd : rvd;
			struct comp_swapchain *sc = (view == 0) ? lsc : rsc;

			VkImageLayout srcImageOldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			VkImage srcImage = sc->vkic.images[data->sub.image_index].handle;

			VkImageSubresourceRange view_subresource_range = {
			    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			    .baseMipLevel = 0,
			    .levelCount = 1,
			    .baseArrayLayer = data->sub.array_index,
			    .layerCount = 1,
			};

			// Barrier to make source back what it was before
			vk_cmd_image_barrier_locked(              //
			    vk,                                   // vk_bundle
			    cmd,                                  // cmdbuffer
			    srcImage,                             // image
			    VK_ACCESS_TRANSFER_READ_BIT,          // srcAccessMask
			    VK_ACCESS_SHADER_READ_BIT,            // dstAccessMask
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // oldImageLayout
			    srcImageOldLayout,                    // newImageLayout
			    VK_PIPELINE_STAGE_TRANSFER_BIT,       // srcStageMask
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask
			    view_subresource_range);              // subresourceRange
		}

		VkImageSubresourceRange first_color_level_subresource_range = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .baseMipLevel = 0,
		    .levelCount = 1,
		    .baseArrayLayer = 0,
		    .layerCount = 1,
		};

		// Barrier transfer image to host so we can safely read back.
		vk_cmd_image_barrier_locked(              //
		    vk,                                   // vk_bundle
		    cmd,                                  // cmdbuffer
		    wrap->image,                          // image
		    VK_ACCESS_TRANSFER_WRITE_BIT,         // srcAccessMask
		    VK_ACCESS_HOST_READ_BIT,              // dstAccessMask
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // oldImageLayout
		    VK_IMAGE_LAYOUT_GENERAL,              // newImageLayout
		    VK_PIPELINE_STAGE_TRANSFER_BIT,       // srcStageMask
		    VK_PIPELINE_STAGE_HOST_BIT,           // dstStageMask
		    first_color_level_subresource_range); // subresourceRange
	}

	// Done submitting commands.

	// Waits for command to finish.
	ret = vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked(vk, &c->cmd_pool, cmd);

	// Unlock before checking.
	vk_cmd_pool_unlock(&c->cmd_pool);

	// Do checking here.
	if (ret != VK_SUCCESS) {
		EMS_COMP_ERROR(c, "vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked: %s", vk_result_string(ret));
		xrt_frame_reference(&frame, NULL);
		return;
	}

	// HACK
	wrap->base_frame.timestamp = os_monotonic_get_ns();
	wrap->base_frame.source_timestamp = wrap->base_frame.timestamp;
	wrap->base_frame.source_sequence = c->image_sequence++;
	wrap->base_frame.source_id = 0;

	// set the latest Downstream mesg before pushing the frame
	em_proto_DownMessage msg = em_proto_DownMessage_init_default;
	msg.has_frame_data = true;
	msg.frame_data.frame_sequence_id = wrap->base_frame.source_sequence;
	msg.frame_data.has_P_localSpace_view0 = true;
	msg.frame_data.P_localSpace_view0 = to_proto(lvd->pose);
	msg.frame_data.has_P_localSpace_view1 = true;
	msg.frame_data.P_localSpace_view1 = to_proto(rvd->pose);
	// msg.frame_datadisplay_time; /* Needed ?*/

	wrap = NULL; // important to keep this line after setting "msg.frame_sequence_id" above.

	ems_gstreamer_pipeline_set_down_msg(c->gstreamer_pipeline, &msg);

	if (!c->pipeline_playing) {
		ems_gstreamer_pipeline_play(c->gstreamer_pipeline);
		c->pipeline_playing = true;
	}

	u_sink_debug_push_frame(&c->debug_sink, frame);

	xrt_sink_push_frame(c->frame_sink, frame);


	// TODO send data channel message with pose and fov here?

	// Dereference this frame - by now we should have pushed it.
	xrt_frame_reference(&frame, NULL);
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
ems_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_DEBUG(c, "BEGIN_SESSION");

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_end_session(struct xrt_compositor *xc)
{
	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_DEBUG(c, "END_SESSION");

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             uint64_t *out_wake_time_ns,
                             uint64_t *out_predicted_gpu_time_ns,
                             uint64_t *out_predicted_display_time_ns,
                             uint64_t *out_predicted_display_period_ns)
{
	COMP_TRACE_MARKER();

	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "PREDICT_FRAME");

	uint64_t now_ns = os_monotonic_get_ns();
	uint64_t null_desired_present_time_ns = 0;
	uint64_t null_present_slop_ns = 0;
	uint64_t null_min_display_period_ns = 0;

	u_pc_predict(                        //
	    c->upc,                          // upc
	    now_ns,                          // now_ns
	    out_frame_id,                    // out_frame_id
	    out_wake_time_ns,                // out_wake_up_time_ns
	    &null_desired_present_time_ns,   // out_desired_present_time_ns
	    &null_present_slop_ns,           // out_present_slop_ns
	    out_predicted_display_time_ns,   // out_predicted_display_time_ns
	    out_predicted_display_period_ns, // out_predicted_display_period_ns
	    &null_min_display_period_ns);    // out_min_display_period_ns

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          uint64_t when_ns)
{
	COMP_TRACE_MARKER();

	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "MARK_FRAME %i", point);

	switch (point) {
	case XRT_COMPOSITOR_FRAME_POINT_WOKE:
		u_pc_mark_point(c->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		return XRT_SUCCESS;
	default: assert(false);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "BEGIN_FRAME");

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "DISCARD_FRAME");

	// Shouldn't be called.
	assert(false);

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	COMP_TRACE_MARKER();

	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "LAYER_COMMIT");

	int64_t frame_id = c->base.slot.data.frame_id;

	u_graphics_sync_unref(&sync_handle);

	/*
	 * Time keeping needed to keep the pacer happy.
	 */

	// When we begin rendering.
	{
		uint64_t now_ns = os_monotonic_get_ns();
		u_pc_mark_point(c->upc, U_TIMING_POINT_BEGIN, frame_id, now_ns);
	}

	// We want to render here. comp_base filled c->base.slot.layers for us.
	for (uint32_t i = 0; i < c->base.slot.layer_count; i++) {
		comp_layer &layer = c->base.slot.layers[i];

		switch (layer.data.type) {
		case XRT_LAYER_STEREO_PROJECTION_DEPTH: {
			const struct xrt_layer_stereo_projection_depth_data *stereo = &layer.data.stereo_depth;
			const struct xrt_layer_projection_view_data *lvd = &stereo->l;
			const struct xrt_layer_projection_view_data *rvd = &stereo->r;

			struct comp_swapchain *left = layer.sc_array[0];
			struct comp_swapchain *right = layer.sc_array[1];

			pack_blit_and_encode(c, lvd, rvd, left, right);
		} break;
		case XRT_LAYER_STEREO_PROJECTION: {
			const struct xrt_layer_stereo_projection_data *stereo = &layer.data.stereo;
			const struct xrt_layer_projection_view_data *lvd = &stereo->l;
			const struct xrt_layer_projection_view_data *rvd = &stereo->r;

			struct comp_swapchain *left = layer.sc_array[0];
			struct comp_swapchain *right = layer.sc_array[1];

			pack_blit_and_encode(c, lvd, rvd, left, right);
		} break;
		default: U_LOG_E("Unhandled layer type %d", layer.data.type); break;
		}
	}

	// When we are submitting to the GPU.
	{
		uint64_t now_ns = os_monotonic_get_ns();
		u_pc_mark_point(c->upc, U_TIMING_POINT_SUBMIT, frame_id, now_ns);
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&c->base.cscs);

	return XRT_SUCCESS;
}


static xrt_result_t
ems_compositor_poll_events(struct xrt_compositor *xc, union xrt_compositor_event *out_xce)
{
	struct ems_compositor *c = ems_compositor(xc);
	EMS_COMP_TRACE(c, "POLL_EVENTS");

	/*
	 * Note this is very often consumed only by the multi compositor.
	 */

	U_ZERO(out_xce);

	switch (c->state) {
	case EMS_COMP_COMP_STATE_UNINITIALIZED:
		EMS_COMP_ERROR(c, "Polled uninitialized compositor");
		out_xce->state.type = XRT_COMPOSITOR_EVENT_NONE;
		break;
	case EMS_COMP_COMP_STATE_READY: out_xce->state.type = XRT_COMPOSITOR_EVENT_NONE; break;
	case EMS_COMP_COMP_STATE_PREPARED:
		EMS_COMP_DEBUG(c, "PREPARED -> VISIBLE");
		out_xce->state.type = XRT_COMPOSITOR_EVENT_STATE_CHANGE;
		out_xce->state.visible = true;
		c->state = EMS_COMP_COMP_STATE_VISIBLE;
		break;
	case EMS_COMP_COMP_STATE_VISIBLE:
		EMS_COMP_DEBUG(c, "VISIBLE -> FOCUSED");
		out_xce->state.type = XRT_COMPOSITOR_EVENT_STATE_CHANGE;
		out_xce->state.visible = true;
		out_xce->state.focused = true;
		c->state = EMS_COMP_COMP_STATE_FOCUSED;
		break;
	case EMS_COMP_COMP_STATE_FOCUSED:
		// No more transitions.
		out_xce->state.type = XRT_COMPOSITOR_EVENT_NONE;
		break;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
ems_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xrt_result_t xret = comp_swapchain_get_create_properties(info, xsccp);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_TRANSFER_SRC | xsccp->extra_bits);

	return XRT_SUCCESS;
}

static void
ems_compositor_destroy(struct xrt_compositor *xc)
{
	struct ems_compositor *c = ems_compositor(xc);
	struct vk_bundle *vk = get_vk(c);

	EMS_COMP_DEBUG(c, "EMS_COMP_COMP_DESTROY");

	// Make sure we don't have anything to destroy.
	comp_swapchain_shared_garbage_collect(&c->base.cscs);
	comp_swapchain_shared_destroy(&c->base.cscs, vk);

	vk_image_readback_to_xf_pool_destroy(vk, &c->pool);

	vk_cmd_pool_destroy(vk, &c->cmd_pool);

	if (c->bounce.image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->bounce.image, NULL);
		vk->vkFreeMemory(vk->device, c->bounce.device_memory, NULL);
		c->bounce.image = VK_NULL_HANDLE;
		c->bounce.device_memory = VK_NULL_HANDLE;
	}

	if (vk->device != VK_NULL_HANDLE) {
		vk->vkDestroyDevice(vk->device, NULL);
		vk->device = VK_NULL_HANDLE;
	}

	vk_deinit_mutex(vk);

	if (vk->instance != VK_NULL_HANDLE) {
		vk->vkDestroyInstance(vk->instance, NULL);
		vk->instance = VK_NULL_HANDLE;
	}

	comp_base_fini(&c->base);

	u_pc_destroy(&c->upc);

	free(c);
}


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
ems_compositor_create_system(ems_instance &emsi, struct xrt_system_compositor **out_xsysc)
{
	struct ems_compositor *c = U_TYPED_CALLOC(struct ems_compositor);

	EMS_COMP_DEBUG(c, "Doing init %p", (void *)c);

	// Needs to be done before functions are set as override function(s).
	comp_base_init(&c->base);

	c->base.base.base.get_swapchain_create_properties = ems_compositor_get_swapchain_create_properties;
	c->base.base.base.begin_session = ems_compositor_begin_session;
	c->base.base.base.end_session = ems_compositor_end_session;
	c->base.base.base.predict_frame = ems_compositor_predict_frame;
	c->base.base.base.mark_frame = ems_compositor_mark_frame;
	c->base.base.base.begin_frame = ems_compositor_begin_frame;
	c->base.base.base.discard_frame = ems_compositor_discard_frame;
	c->base.base.base.layer_commit = ems_compositor_layer_commit;
	c->base.base.base.poll_events = ems_compositor_poll_events;
	c->base.base.base.destroy = ems_compositor_destroy;

	// Note that we don't want to set eg. layer_stereo_projection - comp_base handles that stuff for us.
	c->settings.log_level = debug_get_log_option_log();
	c->frame.waited.id = -1;
	c->frame.rendering.id = -1;
	c->state = EMS_COMP_COMP_STATE_READY;

	xrt_device *xdev = emsi.xsysd_base.roles.head;

	c->settings.frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	c->xdev = xdev;

	EMS_COMP_INFO(c, "Starting Electric Maple Server remote compositor!");


	/*
	 * Main init sequence.
	 */

	if (!compositor_init_pacing(c) ||         //
	    !compositor_init_vulkan(c) ||         //
	    !compositor_init_sys_info(c, xdev) || //
	    !compositor_init_info(c)) {           //
		EMS_COMP_DEBUG(c, "Failed to init compositor %p", (void *)c);
		c->base.base.base.destroy(&c->base.base.base);

		return XRT_ERROR_VULKAN;
	}

	VkExtent2D readback_extent = {};
	readback_extent.height = READBACK_H;
	readback_extent.width = READBACK_W;

	vk_image_readback_to_xf_pool_create( //
	    &c->base.vk,                     // vk_bundle
	    readback_extent,                 // extent
	    &c->pool,                        // out_pool
	    XRT_FORMAT_R8G8B8X8,             // xrt_format
	    VK_FORMAT_R8G8B8A8_UNORM);       // vk_format

	u_var_add_root(c, "Electric Maple Server compositor", 0);
	u_var_add_sink_debug(c, &c->debug_sink, "Debug Sink");

#define EMS_APPSRC_NAME "EMS_source"

	ems_gstreamer_pipeline_create(&c->xfctx, EMS_APPSRC_NAME, emsi.callbacks, &c->gstreamer_pipeline);
	gstreamer_sink_create_with_pipeline( //
	    c->gstreamer_pipeline,           //
	    READBACK_W,                      //
	    READBACK_H,                      //
	    XRT_FORMAT_R8G8B8X8,             //
	    EMS_APPSRC_NAME,                 //
	    &c->gstreamer_sink,              //
	    &c->frame_sink);                 //


	// Bounce image for scaling.
	{
		VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
		VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkExtent2D extent = {READBACK_W, READBACK_H};
		VkResult ret;

		ret = vk_create_image_simple( //
		    &c->base.vk,              // vk_bundle
		    extent,                   // extent
		    format,                   // format
		    usage,                    // usage
		    &c->bounce.device_memory, // out_mem
		    &c->bounce.image);        // out_image
		if (ret != VK_SUCCESS) {
			EMS_COMP_DEBUG(c, "vk_create_image_simple: %s", vk_result_string(ret));
		}
	}

	EMS_COMP_DEBUG(c, "Done %p", (void *)c);

	// Standard app pacer.
	struct u_pacing_app_factory *upaf = NULL;
	XRT_MAYBE_UNUSED xrt_result_t xret = u_pa_factory_create(&upaf);
	assert(xret == XRT_SUCCESS && upaf != NULL);

	return comp_multi_create_system_compositor(&c->base.base, upaf, &c->sys_info, false, out_xsysc);
}
