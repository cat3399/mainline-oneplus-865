// SPDX-License-Identifier: GPL-2.0-only
/* Test the exact classifier, debouncer and codecs compiled into the drivers. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "../../drivers/input/misc/dual-hall-slider.h"
#include "../../drivers/iio/magnetometer/mxm1120-codec.h"

static const struct hall_slider_parameters parameters = {
	.threshold = 30,
	.hysteresis = 8,
	.interference = 40,
};

static const struct hall_slider_calibration calibration = {
	.raw = { { 140, 20 }, { 80, 80 }, { 20, 140 } },
	.valid = true,
};

static void test_positions(void)
{
	struct hall_slider_calibration cal = calibration, none = { 0 };
	int raw[2];
	unsigned int i, channel;

	assert(!hall_slider_validate_calibration(&cal, &parameters));
	for (i = 0; i < HALL_SLIDER_POSITIONS; i++) {
		assert(hall_slider_classify(cal.raw[i], -1, &parameters, &cal) == (int)i);
		assert(hall_slider_classify(cal.raw[i], -1, &parameters, &none) == (int)i);
		raw[0] = cal.raw[i][0] + 3;
		raw[1] = cal.raw[i][1] - 3;
		assert(hall_slider_classify(raw, i, &parameters, &cal) == (int)i);
	}
	for (i = 0; i < HALL_SLIDER_POSITIONS; i++)
		for (channel = 0; channel < HALL_SLIDER_CHANNELS; channel++)
			cal.raw[i][channel] = -cal.raw[i][channel];
	assert(!hall_slider_validate_calibration(&cal, &parameters));
	for (i = 0; i < HALL_SLIDER_POSITIONS; i++) {
		assert(hall_slider_classify(cal.raw[i], -1, &parameters, &cal) == (int)i);
		assert(hall_slider_classify(cal.raw[i], -1, &parameters, &none) == (int)i);
	}
	cal = calibration;
	cal.raw[1][0] = 140;
	cal.raw[1][1] = 20;
	assert(hall_slider_validate_calibration(&cal, &parameters) == -EINVAL);
	cal = calibration;
	cal.raw[2][0] = 160;
	cal.raw[2][1] = 0;
	assert(hall_slider_validate_calibration(&cal, &parameters) == -EINVAL);

	/* Full-width signed IIO counts must not overflow intermediate arithmetic. */
	cal.raw[0][0] = INT_MAX;
	cal.raw[0][1] = INT_MIN;
	cal.raw[1][0] = 0;
	cal.raw[1][1] = 0;
	cal.raw[2][0] = INT_MIN;
	cal.raw[2][1] = INT_MAX;
	assert(!hall_slider_validate_calibration(&cal, &parameters));
	for (i = 0; i < HALL_SLIDER_POSITIONS; i++)
		assert(hall_slider_classify(cal.raw[i], -1, &parameters, &cal) == (int)i);
}

static void test_interference_and_hysteresis(void)
{
	const struct hall_slider_calibration none = { 0 };
	int raw[2] = { 240, 120 };

	assert(hall_slider_classify(raw, 0, &parameters, &calibration) == -1);
	raw[0] = 300;
	raw[1] = -300;
	assert(hall_slider_classify(raw, 0, &parameters, &calibration) == -1);
	raw[0] = 106;
	raw[1] = 54;
	assert(hall_slider_classify(raw, 0, &parameters, &calibration) == 1);
	raw[0] = 107;
	raw[1] = 53;
	assert(hall_slider_classify(raw, 0, &parameters, &calibration) == 0);
	raw[0] = 114;
	raw[1] = 46;
	assert(hall_slider_classify(raw, 1, &parameters, &calibration) == 0);
	raw[0] = 113;
	raw[1] = 47;
	assert(hall_slider_classify(raw, 1, &parameters, &calibration) == 1);
	raw[0] = 103;
	raw[1] = 80;
	assert(hall_slider_classify(raw, 0, &parameters, &none) == 0);
	raw[0] = 102;
	assert(hall_slider_classify(raw, 0, &parameters, &none) == 1);
	raw[0] = 118;
	assert(hall_slider_classify(raw, 1, &parameters, &none) == 1);
	raw[0] = 119;
	assert(hall_slider_classify(raw, 1, &parameters, &none) == 0);
	raw[0] = 57;
	assert(hall_slider_classify(raw, 2, &parameters, &none) == 2);
	raw[0] = 58;
	assert(hall_slider_classify(raw, 2, &parameters, &none) == 1);
}

static void test_debounce(void)
{
	struct hall_slider_state state = { .position = -1, .pending = -1 };

	assert(!hall_slider_update(&state, 0, 0, 100));
	assert(!hall_slider_update(&state, 0, 99, 100));
	assert(hall_slider_update(&state, 0, 100, 100));
	assert(!hall_slider_update(&state, 0, 200, 100));
	assert(!hall_slider_update(&state, 1, 201, 100));
	assert(!hall_slider_update(&state, 2, 250, 100));
	assert(!hall_slider_update(&state, 2, 300, 100));
	assert(hall_slider_update(&state, 2, 350, 100));
	assert(!hall_slider_update(&state, 1, 400, 100));
	assert(!hall_slider_update(&state, -1, 450, 100));
	assert(state.position == 2 && state.pending == -1);
	assert(!hall_slider_update(&state, 1, 500, 100));
	assert(!hall_slider_update(&state, 1, 550, 100));
	assert(hall_slider_update(&state, 1, 600, 100));
	assert(!hall_slider_update(&state, 0, ULONG_MAX - 50, 100));
	assert(!hall_slider_update(&state, 0, 48, 100));
	assert(hall_slider_update(&state, 0, 49, 100));
}

static void test_codecs(void)
{
	int value;
	unsigned char low, high;

	for (value = -512; value <= 511; value++) {
		low = value;
		high = ((value & 0x100) >> 2) | (value < 0 ? 0x80 : 0);
		assert(mxm1120_decode(low, high) == value);
		assert(mxm1120_decode(low, high | 0x3f) == value);
	}
	for (value = -32768; value <= 32767; value++) {
		low = value;
		high = ((unsigned int)value) >> 8;
		assert(ist8801_decode(low, high) == value);
	}
}

static void test_all_raw_pairs(void)
{
	const struct hall_slider_calibration none = { 0 };
	int raw[2], previous, position;

	for (raw[0] = -512; raw[0] <= 511; raw[0]++)
		for (raw[1] = -512; raw[1] <= 511; raw[1]++)
			for (previous = -1; previous < HALL_SLIDER_POSITIONS; previous++) {
				position = hall_slider_classify(raw, previous, &parameters, &calibration);
				assert(position >= -1 && position <= 2);
				position = hall_slider_classify(raw, previous, &parameters, &none);
				assert(position >= -1 && position <= 2);
			}
}

int main(void)
{
	test_positions();
	test_interference_and_hysteresis();
	test_debounce();
	test_codecs();
	test_all_raw_pairs();
	puts("PASS: positions, both polarities, calibration validation, hysteresis,");
	puts("      interference, debounce/wraparound, full-width IIO arithmetic,");
	puts("      all MXM/IST encodings and all 1048576 10-bit raw pairs");
	return 0;
}
