// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  For generating Pluto Pacer.
 * @author Frederic Plourde <frederic.plourde@collabora.com>
 * @ingroup aux_util
 */

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_time.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_pacing.h"
#include "util/u_metrics.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include <stdio.h>
#include <assert.h>
#include <inttypes.h>

// defines for controlling important Pacer parameters
#define PREDICTED_FRAMES_RING_BUFFER_SIZE 32
#define DISPLAY_PERIOD 16666666
#define DECODER_END_TO_BEGIN_FRAME_DELAY_NS 2000000
#define ESTIMATED_FIRST_WAKEUP_DELAY_NS 50000000
#define ESTIMATED_FIRST_PREDICTION_NS 50000000
#define FEEDBACK_FACTOR 2.0f
#define FEEDBACK_PENALITY 1.2f

/*
 *
 * Structs and defines.
 *
 */

struct predicted_frame
{
	/*!
	 * This predicted frame's unique id
	 * Tristate : -1 = not assigned.
	 */
	int64_t id;

	/*!
	 * Wake up time for this predicted frame
	 */
	uint64_t wake_time_ns;

	/*!
	 * Server-side display-time
	 * This is monado's out_predicted_display_time_ns,
	 * but we are remote-rendering here so we are just
	 * mimicking timing on client side and incrementing
	 * by out_predicted_display_period_ns;
	 */
	uint64_t server_display_time_ns;

	/*!
	 * Client-side display-time
	 * This is messaged client-side display time (in
	 * client clock domain) for upstream frame used/
	 * corresponding to this predicted_frame.
	 * Note: Tristate : -1 = no associated client frame.
	 */

	// FIXME: casting nanoseconds in this int64_t for
	//        having a tristate will keep working reliably
	//        until year 2262 where we'll blow up the int64
	//        range. Find a better struct/type before then !
	int64_t client_display_time_ns;
};

// Client feedback
struct client_feedback
{
	/*!
	 * Offset reported/calculated from last client feedback.
	 *
	 * This tells us if we need to speed up frame cycle (neg. values)
	 * or slow it down (pos. values)
	 */
	int64_t wakeup_offset;

	/*!
	 * Display time reported from last client feedback.
	 * Tristate: -1 = haven't received any report.
	 */
	int64_t client_display_time;
};

struct pluto_timing
{
	struct u_pacing_compositor base;

	// our predicted frames
	struct predicted_frame predicted_frames[PREDICTED_FRAMES_RING_BUFFER_SIZE];

	// last frame we predicted
	int64_t last_id;

	// last client feedback
	struct client_feedback last_client_feedback;

	/*!
	 * Number of frames we have missed since last prediction.
	 * we deduct that number from wake-up cycles we have to add to reach now.
	 * Tristate : missed_frames = -1 says we don't know yet.
	 */
	int64_t missed_frames;

	// For avoiding processing new feedback while predicting
	struct os_mutex feedback_lock;

	// We are updating this at every ::predict call.
	int64_t client_to_server_time_offset_ns;

};

/*
 *
 * Helper functions.
 *
 */

static inline struct pluto_timing *
pluto_timing(struct u_pacing_compositor *upc)
{
	return (struct pluto_timing *)upc;
}

// Next wakeup time is 1 display period after last wakeup time plus/minus some feedback-driven offset.
static uint64_t
predict_next_frame_wake_up_time(struct pluto_timing *ft, int64_t last, int64_t next, int64_t offset, uint64_t now_ns)
{
	// First frame ?
	if (-1 == ft->predicted_frames[last].id) {
		// last frame is always assigned, unless it's first frame, then assign it.
		ft->predicted_frames[last].id = ft->last_id;
		// we don't have a choice... those seems like the most logical init values ?
		ft->predicted_frames[last].wake_time_ns = now_ns;
		ft->predicted_frames[last].server_display_time_ns = now_ns + ESTIMATED_FIRST_WAKEUP_DELAY_NS;
	}

	ft->predicted_frames[next].id = ft->predicted_frames[last].id + 1;

	// Offset was set from last client feedback.
	// Note: if we missed a frame on client-side, adding 1 display_period here
	// to last wake-up time will still be in the past ! So do the below logic
	// until wake_time_ns >= now_ns.
	ft->predicted_frames[next].wake_time_ns = ft->predicted_frames[last].wake_time_ns;
	ft->missed_frames = -1;
	do {
		ft->predicted_frames[next].wake_time_ns += (DISPLAY_PERIOD + offset);
		ft->missed_frames++;
	} while (ft->predicted_frames[next].wake_time_ns < now_ns);

	return ft->predicted_frames[next].wake_time_ns;
}

// Next client display time is last client **actual** (feedback) display
// time plus 1 display period.
static void
predict_next_frame_client_present_time(struct pluto_timing *ft, int64_t last, int64_t next, uint64_t now_ns)
{

	if (-1 == ft->last_client_feedback.client_display_time) {
		// no report yet. We can't calculate next client display time :(
		ft->predicted_frames[next].client_display_time_ns = -1;
	} else if ((-1 != ft->last_client_feedback.client_display_time) &&
			   (-1 == ft->predicted_frames[last].client_display_time_ns)) {
		// no 'last' client display time. we'll have to pick an initial display time.
		ft->predicted_frames[next].client_display_time_ns =
				ft->last_client_feedback.client_display_time + ESTIMATED_FIRST_PREDICTION_NS;
	} else {
		// typical case, we start from our last *actual* client display time
		// and add periods until we match last frame's client display time + 1period.
		ft->predicted_frames[next].client_display_time_ns = ft->last_client_feedback.client_display_time;
		do {
			ft->predicted_frames[next].client_display_time_ns += DISPLAY_PERIOD;
		} while (ft->predicted_frames[next].client_display_time_ns >=
				ft->predicted_frames[last].client_display_time_ns +
				(ft->missed_frames * DISPLAY_PERIOD));
	}

	// Now is a good time to compute our client-to-server time difference
	ft->client_to_server_time_offset_ns = ft->predicted_frames[next].server_display_time_ns -
										  ft->predicted_frames[next].client_display_time_ns;

	// we're not returning anything as next client_display_time does not get out of ::predict.
}

// We just follow client side here, let's just add the client interval to our server
// display time.
static uint64_t
predict_next_frame_server_present_time(struct pluto_timing *ft, int64_t last, int64_t next, uint64_t now_ns)
{
	ft->predicted_frames[next].server_display_time_ns =
			ft->predicted_frames[last].server_display_time_ns + (ft->predicted_frames[next].client_display_time_ns -
					ft->predicted_frames[last].client_display_time_ns);
	// again, we might have had missed frame, compensate !
	while (ft->predicted_frames[next].server_display_time_ns < ft->predicted_frames[next].wake_time_ns) {
		ft->predicted_frames[next].server_display_time_ns += DISPLAY_PERIOD;
	}

	return ft->predicted_frames[next].server_display_time_ns;
}

/*
 *
 * Member functions.
 *
 */

static void
pc_predict(struct u_pacing_compositor *upc,
           uint64_t now_ns,
           int64_t *out_frame_id,
           uint64_t *out_wake_up_time_ns,
           uint64_t *out_desired_present_time_ns,
           uint64_t *out_present_slop_ns,
           uint64_t *out_predicted_display_time_ns,
           uint64_t *out_predicted_display_period_ns,
           uint64_t *out_min_display_period_ns)
{
	struct pluto_timing *ft = pluto_timing(upc);

	os_mutex_lock(&ft->feedback_lock);

	// There's a 'last' and a 'next' frame. We are working on the 'next' one.
	// Next frame's id will be incremented from last frame id. Get those.
	// note: 'last' and 'next' are indexes inside predicted_frames array
	//       and thus clamped to [0, PREDICTED_FRAMES_RING_BUFFER_SIZE[
	int64_t last = ft->last_id % PREDICTED_FRAMES_RING_BUFFER_SIZE;
	int64_t next = (last + 1) %  PREDICTED_FRAMES_RING_BUFFER_SIZE;

	uint64_t wake_up_time_ns = predict_next_frame_wake_up_time(ft, last, next, ft->last_client_feedback.wakeup_offset, now_ns);
	(void) predict_next_frame_client_present_time(ft, last, next, now_ns);
	uint64_t predicted_server_display_time_ns = predict_next_frame_server_present_time(ft, last, next, now_ns);
	uint64_t desired_present_time_ns = predicted_server_display_time_ns;
	uint64_t present_slop_ns = U_TIME_HALF_MS_IN_NS;
	uint64_t predicted_display_period_ns = DISPLAY_PERIOD;
	uint64_t min_display_period_ns = DISPLAY_PERIOD;

	// TODO: Make SURE this gets added to ft->last_id *and* stored(assigned).
	*out_frame_id = ++ft->last_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_present_slop_ns = present_slop_ns;
	*out_predicted_display_time_ns = predicted_server_display_time_ns;
	*out_predicted_display_period_ns = predicted_display_period_ns;
	*out_min_display_period_ns = min_display_period_ns;

	// we're done, let's reset some state
	ft->last_client_feedback = {0, -1}; //we're done consuming that client feedback.

	// !QUESTION: Do we need u_metrics_is_active() logic in prediction code ?
	/*
	if (!u_metrics_is_active()) {
		return;
	}

	struct u_metrics_system_frame umsf = {
	    .frame_id = frame_id,
	    .predicted_display_time_ns = predicted_display_time_ns,
	    .predicted_display_period_ns = predicted_display_period_ns,
	    .desired_present_time_ns = desired_present_time_ns,
	    .wake_up_time_ns = wake_up_time_ns,
	    .present_slop_ns = present_slop_ns,
	};

	u_metrics_write_system_frame(&umsf);
	*/
	os_mutex_unlock(&ft->feedback_lock);
}

static void
pc_mark_point(struct u_pacing_compositor *upc, enum u_timing_point point, int64_t frame_id, uint64_t when_ns)
{
	// To help validate calling code.
	switch (point) {
	case U_TIMING_POINT_WAKE_UP: break;
	case U_TIMING_POINT_BEGIN: break;
	case U_TIMING_POINT_SUBMIT: break;
	default: assert(false);
	}
}

static void
pc_info(struct u_pacing_compositor *upc,
        int64_t frame_id,
        uint64_t desired_present_time_ns,
        uint64_t actual_present_time_ns,
        uint64_t earliest_present_time_ns,
        uint64_t present_margin_ns,
        uint64_t when_ns)
{
	/*
	 * The compositor might call this function because it selected the
	 * fake timing code even tho displaying timing is available.
	 */
}

static void
pc_info_gpu(
    struct u_pacing_compositor *upc, int64_t frame_id, uint64_t gpu_start_ns, uint64_t gpu_end_ns, uint64_t when_ns)
{
	// !QUESTION: Is pc_info_gpu implementation mandatory ?
	/* if (u_metrics_is_active()) {
		struct u_metrics_system_gpu_info umgi = {
		    .frame_id = frame_id,
		    .gpu_start_ns = gpu_start_ns,
		    .gpu_end_ns = gpu_end_ns,
		    .when_ns = when_ns,
		};

		u_metrics_write_system_gpu_info(&umgi);
	}

#ifdef U_TRACE_PERCETTO // Uses Perfetto specific things.
	if (U_TRACE_CATEGORY_IS_ENABLED(timing)) {
#define TE_BEG(TRACK, TIME, NAME) U_TRACE_EVENT_BEGIN_ON_TRACK_DATA(timing, TRACK, TIME, NAME, PERCETTO_I(frame_id))
#define TE_END(TRACK, TIME) U_TRACE_EVENT_END_ON_TRACK(timing, TRACK, TIME)

		TE_BEG(pc_gpu, gpu_start_ns, "gpu");
		TE_END(pc_gpu, gpu_end_ns);

#undef TE_BEG
#undef TE_END
	}
#endif

#ifdef U_TRACE_TRACY
	uint64_t diff_ns = gpu_end_ns - gpu_start_ns;
	TracyCPlot("Compositor GPU(ms)", time_ns_to_ms_f(diff_ns));
#endif
*/
}

static void
pc_update_vblank_from_display_control(struct u_pacing_compositor *upc, uint64_t last_vblank_ns)
{
	return; // NOOP
}

static void
pc_update_present_offset(struct u_pacing_compositor *upc, int64_t frame_id, uint64_t present_to_display_offset_ns)
{
	return; // NOOP
}

static void
pc_destroy(struct u_pacing_compositor *upc)
{
	struct pluto_timing *ft = pluto_timing(upc);
	os_mutex_destroy(&ft->feedback_lock);
	free(ft);
}

static int64_t
compute_offset(int64_t error)
{
	float ret_feedback = (float) error * FEEDBACK_FACTOR;

	if (error > 0) {
		// Client side was 'late'. Penalize this even more.
		ret_feedback *= FEEDBACK_PENALITY;
	}

	return (int64_t) ret_feedback;
}

/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
u_pc_pluto_create(uint64_t estimated_frame_period_ns, uint64_t now_ns, struct u_pacing_compositor **out_upc)
{
	struct pluto_timing *ft = U_TYPED_CALLOC(struct pluto_timing);
	os_mutex_init(&ft->feedback_lock);
	ft->base.predict = pc_predict;
	ft->base.mark_point = pc_mark_point;
	ft->base.info = pc_info;
	ft->base.info_gpu = pc_info_gpu;
	ft->base.update_vblank_from_display_control = pc_update_vblank_from_display_control;
	ft->base.update_present_offset = pc_update_present_offset;
	ft->base.destroy = pc_destroy;

	// Populate predicted_frames
	for (int i = 0; i < PREDICTED_FRAMES_RING_BUFFER_SIZE; i++) {
		// all frames are initially unassigned (-1) and not associated with upstream (-1)
		ft->predicted_frames[i] = {-1, 0, 0, -1};
	}

	// Let's set first 'last_id' to something non-zero, just to be safe.
	ft->last_id = 5;

	// Initially, we have no device feedback, so no wake-up time offset.
	ft->last_client_feedback.wakeup_offset = 0;
	ft->last_client_feedback.client_display_time = -1;

	ft->client_to_server_time_offset_ns = 0;

	// Return value.
	*out_upc = &ft->base;

	U_LOG_I("Created Pluto Pacer");

	return XRT_SUCCESS;
}

// Client feedback used to drive our PLL. 'frame_id' is used to find predicted frame
// that we once worked on and that's associated with the reported feedback.
void
u_pc_pluto_give_feedback(struct u_pacing_compositor *upc,
		                 int64_t  reported_frame_id,
						 uint64_t reported_client_decoder_out_time_ns,
						 uint64_t reported_client_begin_frame_time_ns,
						 uint64_t reported_client_display_time_ns)
{
	struct pluto_timing *ft = pluto_timing(upc);

	os_mutex_lock(&ft->feedback_lock);

	// Using reported frame_id, look up corresponding predicted frame and make some sanity checks.
	uint64_t index = reported_frame_id % PREDICTED_FRAMES_RING_BUFFER_SIZE;
	if (ft->predicted_frames[index].id != reported_frame_id) {
		// Something's wrong, we haven't found the right predicted frame. Return.
		return;
	}

	// Compute our reported duration between begin_frame call and display_time.
	uint64_t reported_begin_frame_to_display_ns = reported_client_display_time_ns - reported_client_begin_frame_time_ns;


	// Using this reported begin_frame_to_display duration, compute predicted begin_frame_time_ns.
	uint64_t predicted_begin_frame_time_ns = ft->predicted_frames[index].client_display_time_ns - reported_begin_frame_to_display_ns;

	// We assume the delay between begin_frame_time and decoder_end_time to be a constant DECODER_END_TO_BEGIN_FRAME_DELAY_NS
	// This allows us to compare our reported decoder_end_time to our predicted decoder_end_time and compute our 'error'
	// that is going to drive our PLL.
	int64_t error = predicted_begin_frame_time_ns - DECODER_END_TO_BEGIN_FRAME_DELAY_NS -
					reported_client_decoder_out_time_ns;

	// error is a client-side time difference indicating by how much we missed (pos. or neg)
	// our predicted decoder_end time. We use this diff. in this pacer to drive how much we
	// have to adjust (either neg. or pos.) our next frame's wake-up time, but depending on
	// various factors (e.g. positive (late) values are worse than negative ones), we are adjusting
	// the feedback factor in a smart way here, hence the compute_offset() func.
	ft->last_client_feedback.client_display_time = (int64_t) reported_client_display_time_ns;
	ft->last_client_feedback.wakeup_offset = compute_offset(error);

	os_mutex_unlock(&ft->feedback_lock);
}

int64_t
get_client_to_server_time_offset_ns(struct u_pacing_compositor *upc)
{
	struct pluto_timing *ft = pluto_timing(upc);
	return ft->client_to_server_time_offset_ns;
}
