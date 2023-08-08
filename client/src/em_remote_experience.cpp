// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Implementation for remote experience object
 * @author Ryan Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */

#include "em_remote_experience.h"

#include "GLSwapchain.h"
#include "gst/app_log.h"
#include "gst/em_connection.h"
#include "gst/em_stream_client.h"
#include "gst/gst_common.h"
#include "render.hpp"

#include "pb_encode.h"
#include "pluto.pb.h"

#include "xr_platform_deps.h"

#include <GLES3/gl3.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <openxr/openxr.h>


struct _EmRemoteExperience
{
	EmConnection *connection;
	EmStreamClient *stream_client;
	std::unique_ptr<Renderer> renderer;
	struct em_sample *prev_sample;

	XrExtent2Di eye_extents;

	struct
	{
		XrInstance instance{XR_NULL_HANDLE};
		// XrSystemId system;
		XrSession session{XR_NULL_HANDLE};

	} xr_not_owned;

	struct
	{
		XrSpace worldSpace;
		XrSpace viewSpace;
		XrSwapchain swapchain;
	} xr_owned;

	GLSwapchain swapchainBuffers;
};


static void
em_remote_experience_report_pose(EmRemoteExperience *exp, XrTime predictedDisplayTime)
{
	XrResult result = XR_SUCCESS;


	XrSpaceLocation hmdLocalLocation = {};
	hmdLocalLocation.type = XR_TYPE_SPACE_LOCATION;
	hmdLocalLocation.next = NULL;
	result =
	    xrLocateSpace(exp->xr_owned.viewSpace, exp->xr_owned.worldSpace, predictedDisplayTime, &hmdLocalLocation);
	if (result != XR_SUCCESS) {
		ALOGE("Bad!");
		return;
	}

	XrPosef hmdLocalPose = hmdLocalLocation.pose;

	pluto_TrackingMessage message = pluto_TrackingMessage_init_default;

	message.has_P_localSpace_viewSpace = true;
	message.P_localSpace_viewSpace.has_position = true;
	message.P_localSpace_viewSpace.has_orientation = true;
	message.P_localSpace_viewSpace.position.x = hmdLocalPose.position.x;
	message.P_localSpace_viewSpace.position.y = hmdLocalPose.position.y;
	message.P_localSpace_viewSpace.position.z = hmdLocalPose.position.z;

	message.P_localSpace_viewSpace.orientation.w = hmdLocalPose.orientation.w;
	message.P_localSpace_viewSpace.orientation.x = hmdLocalPose.orientation.x;
	message.P_localSpace_viewSpace.orientation.y = hmdLocalPose.orientation.y;
	message.P_localSpace_viewSpace.orientation.z = hmdLocalPose.orientation.z;

	uint8_t buffer[8192];



	pb_ostream_t os = pb_ostream_from_buffer(buffer, sizeof(buffer));

	pb_encode(&os, pluto_TrackingMessage_fields, &message);
	ALOGW("RYLIE: Sending HMD pose message");
	GBytes *bytes = g_bytes_new(buffer, os.bytes_written);
	bool bResult = em_connection_send_bytes(exp->connection, bytes);
	g_bytes_unref(bytes);
	if (!bResult) {
		ALOGE("RYLIE: Could not queue HMD pose message!");
	}
}

static void
em_remote_experience_dispose(EmRemoteExperience *exp)
{
	if (exp->stream_client) {
		em_stream_client_stop(exp->stream_client);
		if (exp->renderer) {
			em_stream_client_egl_begin_pbuffer(exp->stream_client);
			exp->renderer->reset();
			exp->renderer = nullptr;
			em_stream_client_egl_end(exp->stream_client);
		}
		if (exp->prev_sample) {
			em_stream_client_release_sample(exp->stream_client, exp->prev_sample);
			exp->prev_sample = nullptr;
		}
	}
	if (exp->connection) {
		em_connection_disconnect(exp->connection);
	}
	// stream client is not gobject (yet?)
	em_stream_client_destroy(&exp->stream_client);
	g_clear_object(&exp->connection);
	exp->swapchainBuffers.reset();

	if (exp->renderer) {
		ALOGW(
		    "%s: Renderer outlived stream client somehow (should not happen), "
		    "will take a chance at destroying it anyway",
		    __FUNCTION__);
		exp->renderer->reset();
		exp->renderer = nullptr;
	}
}
static void
em_remote_experience_finalize(EmRemoteExperience *exp)
{
	if (exp->xr_owned.swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(exp->xr_owned.swapchain);
		exp->xr_owned.swapchain = XR_NULL_HANDLE;
	}

	if (exp->xr_owned.viewSpace != XR_NULL_HANDLE) {
		xrDestroySpace(exp->xr_owned.viewSpace);
		exp->xr_owned.viewSpace = XR_NULL_HANDLE;
	}

	if (exp->xr_owned.worldSpace != XR_NULL_HANDLE) {
		xrDestroySpace(exp->xr_owned.worldSpace);
		exp->xr_owned.worldSpace = XR_NULL_HANDLE;
	}
}

EmRemoteExperience *
em_remote_experience_new(EmConnection *connection,
                         EmStreamClient *stream_client,
                         XrInstance instance,
                         XrSession session,
                         const XrExtent2Di *eye_extents)
{
	EmRemoteExperience *self = reinterpret_cast<EmRemoteExperience *>(calloc(1, sizeof(EmRemoteExperience)));
	self->connection = g_object_ref_sink(connection);
	// self->stream_client = g_object_ref_sink(stream_client);
	self->stream_client = stream_client;
	self->eye_extents = *eye_extents;
	self->xr_not_owned.instance = instance;
	self->xr_not_owned.session = session;


	{
		ALOGI("%s: Creating OpenXR Swapchain...", __FUNCTION__);
		// OpenXR swapchain
		XrSwapchainCreateInfo swapchainInfo = {};
		swapchainInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		swapchainInfo.format = GL_SRGB8_ALPHA8;
		swapchainInfo.width = self->eye_extents.width * 2;
		swapchainInfo.height = self->eye_extents.height;
		swapchainInfo.sampleCount = 1;
		swapchainInfo.faceCount = 1;
		swapchainInfo.arraySize = 1;
		swapchainInfo.mipCount = 1;

		XrResult result = xrCreateSwapchain(session, &swapchainInfo, &self->xr_owned.swapchain);

		if (XR_FAILED(result)) {
			ALOGE("%s: Failed to create OpenXR swapchain (%d)\n", __FUNCTION__, result);
			em_remote_experience_destroy(&self);
			return nullptr;
		}
	}
	{
		em_stream_client_egl_begin_pbuffer(stream_client);

		if (!self->swapchainBuffers.enumerateAndGenerateFramebuffers(self->xr_owned.swapchain)) {
			ALOGE(
			    "%s: Failed to enumerate swapchain images or associate them with framebuffer object names.",
			    __FUNCTION__);
			em_stream_client_egl_end(stream_client);
			em_remote_experience_destroy(&self);
			return nullptr;
		}


		try {
			ALOGI("%s: Setup renderer...", __FUNCTION__);
			self->renderer = std::make_unique<Renderer>();
			self->renderer->setupRender();
		} catch (std::exception const &e) {
			ALOGE("%s: Caught exception setting up renderer: %s", __FUNCTION__, e.what());
			self->renderer->reset();
			em_stream_client_egl_end(stream_client);
			em_remote_experience_destroy(&self);
			return nullptr;
		}


		em_stream_client_egl_end(stream_client);
	}
	{
		ALOGI("%s: Creating OpenXR Spaces...", __FUNCTION__);

		XrResult result = XR_SUCCESS;

		XrReferenceSpaceCreateInfo spaceInfo = {
		    .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		    .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE,
		    .poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}}};


		result = xrCreateReferenceSpace(session, &spaceInfo, &self->xr_owned.worldSpace);

		if (XR_FAILED(result)) {
			ALOGE("%s: Failed to create world reference space (%d)", __FUNCTION__, result);
			em_remote_experience_destroy(&self);
			return nullptr;
		}
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;

		result = xrCreateReferenceSpace(session, &spaceInfo, &self->xr_owned.viewSpace);

		if (XR_FAILED(result)) {
			ALOGE("%s: Failed to create view reference space (%d)", __FUNCTION__, result);
			em_remote_experience_destroy(&self);
			return nullptr;
		}
	}


	return self;
}

void
em_remote_experience_destroy(EmRemoteExperience **ptr_exp)
{

	if (ptr_exp == NULL) {
		return;
	}
	EmRemoteExperience *exp = *ptr_exp;
	if (exp == NULL) {
		return;
	}
	em_remote_experience_dispose(exp);
	em_remote_experience_finalize(exp);
	free(exp);
	*ptr_exp = NULL;
}

void
em_remote_experience_poll_and_render_frame(EmRemoteExperience *exp)
{

	XrFrameState frameState = {.type = XR_TYPE_FRAME_STATE};
	XrSession session = exp->xr_not_owned.session;
	XrResult result = xrWaitFrame(session, NULL, &frameState);

	if (XR_FAILED(result)) {
		ALOGE("xrWaitFrame failed");
		return;
	}

	XrFrameBeginInfo beginfo = {.type = XR_TYPE_FRAME_BEGIN_INFO};

	result = xrBeginFrame(session, &beginfo);

	if (XR_FAILED(result)) {
		ALOGE("xrBeginFrame failed");
		std::abort();
	}

	// Locate views, set up layers
	XrView views[2] = {};
	views[0].type = XR_TYPE_VIEW;
	views[1].type = XR_TYPE_VIEW;


	XrViewLocateInfo locateInfo = {.type = XR_TYPE_VIEW_LOCATE_INFO,
	                               .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                               .displayTime = frameState.predictedDisplayTime,
	                               .space = exp->xr_owned.worldSpace};

	XrViewState viewState = {.type = XR_TYPE_VIEW_STATE};

	uint32_t viewCount = 2;
	result = xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views);

	if (XR_FAILED(result)) {
		ALOGE("Failed to locate views");
	}

	// TODO these may not be the extents of the frame we receive, thus introducing repeated scaling!
	uint32_t width = exp->eye_extents.width;
	uint32_t height = exp->eye_extents.height;
	XrCompositionLayerProjection layer = {};
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	layer.space = exp->xr_owned.worldSpace;
	layer.viewCount = 2;

	// TODO use multiview/array swapchain instead of two draw calls for side by side?
	XrCompositionLayerProjectionView projectionViews[2] = {};
	projectionViews[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
	projectionViews[0].subImage.swapchain = exp->xr_owned.swapchain;
	projectionViews[0].subImage.imageRect.offset = {0, 0};
	projectionViews[0].subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
	projectionViews[0].pose = views[0].pose;
	projectionViews[0].fov = views[0].fov;

	projectionViews[1].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
	projectionViews[1].subImage.swapchain = exp->xr_owned.swapchain;
	projectionViews[1].subImage.imageRect.offset = {static_cast<int32_t>(width), 0};
	projectionViews[1].subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
	projectionViews[1].pose = views[1].pose;
	projectionViews[1].fov = views[1].fov;

	layer.views = projectionViews;

	// Render

	if (!em_stream_client_egl_begin_pbuffer(exp->stream_client)) {
		ALOGE("FRED: mainloop_one: Failed make egl context current");
		return;
	}

	struct em_sample *sample = em_stream_client_try_pull_sample(exp->stream_client);

	if (sample) {

		uint32_t imageIndex;
		result = xrAcquireSwapchainImage(exp->xr_owned.swapchain, NULL, &imageIndex);

		if (XR_FAILED(result)) {
			ALOGE("Failed to acquire swapchain image (%d)", result);
			std::abort();
		}

		XrSwapchainImageWaitInfo waitInfo = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
		                                     .timeout = XR_INFINITE_DURATION};

		result = xrWaitSwapchainImage(exp->xr_owned.swapchain, &waitInfo);

		if (XR_FAILED(result)) {
			ALOGE("Failed to wait for swapchain image (%d)", result);
			std::abort();
		}
		glBindFramebuffer(GL_FRAMEBUFFER, exp->swapchainBuffers.framebufferNameAtSwapchainIndex(imageIndex));

		glViewport(0, 0, width * 2, height);
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);

		for (uint32_t eye = 0; eye < 2; eye++) {
			glViewport(eye * width, 0, width, height);
			exp->renderer->draw(sample->frame_texture_id, sample->frame_texture_target);
		}

		// Release

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		xrReleaseSwapchainImage(exp->xr_owned.swapchain, NULL);

		if (exp->prev_sample != NULL) {
			em_stream_client_release_sample(exp->stream_client, exp->prev_sample);
			exp->prev_sample = NULL;
		}
		exp->prev_sample = sample;
		// Submit frame
		XrFrameEndInfo endInfo = {};
		endInfo.type = XR_TYPE_FRAME_END_INFO;
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = frameState.shouldRender ? 1 : 0;
		endInfo.layers = (const XrCompositionLayerBaseHeader *[1]){(XrCompositionLayerBaseHeader *)&layer};

		xrEndFrame(session, &endInfo);
	}
	em_remote_experience_report_pose(exp, frameState.predictedDisplayTime);

	em_stream_client_egl_end(exp->stream_client);
}