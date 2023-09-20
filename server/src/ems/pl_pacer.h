// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for null compositor.
 *
 * Based on src/xrt/compositor/main/comp_compositor.h
 *
 * @author Frederic Plourde <fred23@collabora.com>
 * @ingroup comp_null
 */

#pragma once

#include "util/u_pacing.h"

/*!
 * Creates a new Pluto Pacer.
 *
 * @param[in]  estimated_frame_period_ns The estimated duration/period of a frame in nanoseconds.
 * @param[in]  now_ns                    The current timestamp in nanoseconds, nominally from @ref os_monotonic_get_ns
 * @param[out] out_upc                   The pointer to populate with the created compositor pacing helper
 *
 * @ingroup aux_pacing
 * @see u_pacing_compositor
 */
xrt_result_t
u_pc_pluto_create(uint64_t estimated_frame_period_ns, uint64_t now_ns, struct u_pacing_compositor **out_upc);

/*!
 * Give feedback for controlling Pacer
 *
 * @param[in]  upc the u_pacing_compositor
 * @param[in]  reported_client_decoder_out_time_ns client-side actual decode_out time
 * @param[in]  reported_client_begin_frame_time_ns client-side actual begin frame time
 * @param[in]  reported_client_display_time_ns client-side actual display time
 *
 * @ingroup aux_pacing
 * @see u_pacing_compositor
 */
void
u_pc_pluto_give_feedback(struct u_pacing_compositor *upc,
		                 int64_t  reported_frame_id,
						 uint64_t reported_client_decoder_out_time_ns,
						 uint64_t reported_client_begin_frame_time_ns,
						 uint64_t reported_client_display_time_ns);

/*!
 * Get client-to-server time difference in ns.
 *
 * @param[in]  upc the u_pacing_compositor
 *
 * @ingroup aux_pacing
 * @see u_pacing_compositor
 */
int64_t
get_client_to_server_time_offset_ns(struct u_pacing_compositor *upc);
