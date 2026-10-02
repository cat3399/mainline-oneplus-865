/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DUAL_HALL_SLIDER_H
#define _DUAL_HALL_SLIDER_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

#define HALL_SLIDER_UNKNOWN	(-1)
#define HALL_SLIDER_POSITIONS	3
#define HALL_SLIDER_CHANNELS	2

struct hall_slider_parameters {
	unsigned int threshold;
	unsigned int hysteresis;
	unsigned int interference;
};

struct hall_slider_calibration {
	/* End-0 and end-2 readings at positions 0, 1, and 2, respectively. */
	int raw[HALL_SLIDER_POSITIONS][HALL_SLIDER_CHANNELS];
	bool valid;
};

struct hall_slider_state {
	int position;
	int pending;
	unsigned long pending_since;
};

static inline long long hall_slider_abs(long long value)
{
	return value < 0 ? -value : value;
}

static inline long long hall_slider_delta(const int raw[2])
{
	return (long long)raw[0] - raw[1];
}

static inline long long hall_slider_sum(const int raw[2])
{
	return (long long)raw[0] + raw[1];
}

static inline int
hall_slider_validate_calibration(const struct hall_slider_calibration *cal,
				const struct hall_slider_parameters *parameters)
{
	long long first, second;

	first = hall_slider_delta(cal->raw[0]) - hall_slider_delta(cal->raw[1]);
	second = hall_slider_delta(cal->raw[1]) - hall_slider_delta(cal->raw[2]);
	/* Accept either polarity, but reject duplicate or unordered detents. */
	if (hall_slider_abs(first) <= 2LL * parameters->hysteresis ||
	    hall_slider_abs(second) <= 2LL * parameters->hysteresis ||
	    (first > 0) != (second > 0))
		return -EINVAL;
	return 0;
}

static inline int
hall_slider_classify(const int raw[2], int previous,
		     const struct hall_slider_parameters *parameters,
		     const struct hall_slider_calibration *cal)
{
	long long delta, distance, best = 0, gap, other_gap;
	int position = HALL_SLIDER_UNKNOWN;
	unsigned int i;

	if (!cal->valid) {
		/* Bootstrap for sensors mounted at opposite ends of the travel. */
		delta = hall_slider_abs(raw[0]) - hall_slider_abs(raw[1]);
		if (previous == 0 &&
		    delta > (long long)parameters->threshold - parameters->hysteresis)
			return 0;
		if (previous == 2 &&
		    delta < -(long long)parameters->threshold + parameters->hysteresis)
			return 2;
		distance = parameters->threshold;
		if (previous == 1)
			distance += parameters->hysteresis;
		if (delta > distance)
			return 0;
		if (delta < -distance)
			return 2;
		return 1;
	}

	delta = hall_slider_delta(raw);
	for (i = 0; i < HALL_SLIDER_POSITIONS; i++) {
		distance = hall_slider_abs(delta - hall_slider_delta(cal->raw[i]));
		if (position == HALL_SLIDER_UNKNOWN || distance < best) {
			position = i;
			best = distance;
		}
	}
	if (previous >= 0 && previous < HALL_SLIDER_POSITIONS && previous != position) {
		distance = hall_slider_abs(delta - hall_slider_delta(cal->raw[previous]));
		if (distance - best < 2LL * parameters->hysteresis) {
			position = previous;
			best = distance;
		}
	}

	gap = hall_slider_abs(hall_slider_delta(cal->raw[0]) -
			      hall_slider_delta(cal->raw[1]));
	other_gap = hall_slider_abs(hall_slider_delta(cal->raw[1]) -
				    hall_slider_delta(cal->raw[2]));
	if (other_gap < gap)
		gap = other_gap;
	if (best > gap ||
	    hall_slider_abs(hall_slider_sum(raw) - hall_slider_sum(cal->raw[position])) >
	    parameters->interference)
		return HALL_SLIDER_UNKNOWN;
	return position;
}

/* A failed/ambiguous measurement never fabricates a new position. */
static inline bool hall_slider_update(struct hall_slider_state *state, int sample,
				      unsigned long now, unsigned long debounce)
{
	if (sample == HALL_SLIDER_UNKNOWN || sample == state->position) {
		state->pending = HALL_SLIDER_UNKNOWN;
		return false;
	}
	if (sample != state->pending) {
		state->pending = sample;
		state->pending_since = now;
	}
	/* Unsigned elapsed time also handles a jiffies wraparound. */
	if (now - state->pending_since < debounce)
		return false;
	state->position = sample;
	state->pending = HALL_SLIDER_UNKNOWN;
	return true;
}

#endif /* _DUAL_HALL_SLIDER_H */
