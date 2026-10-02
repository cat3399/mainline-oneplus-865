/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SYNAPTICS_TCM_REPORT_H
#define _SYNAPTICS_TCM_REPORT_H

#include <linux/align.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#define TCM_MAX_REPORT_CONFIG_SIZE	256
#define TCM_MAX_OBJECTS			32

enum tcm_touch_report_code {
	TOUCH_END = 0x00,
	TOUCH_FOREACH_ACTIVE_OBJECT = 0x01,
	TOUCH_FOREACH_OBJECT = 0x02,
	TOUCH_FOREACH_END = 0x03,
	TOUCH_PAD_TO_NEXT_BYTE = 0x04,
	TOUCH_OBJECT_N_INDEX = 0x06,
	TOUCH_OBJECT_N_CLASSIFICATION = 0x07,
	TOUCH_OBJECT_N_X_POSITION = 0x08,
	TOUCH_OBJECT_N_Y_POSITION = 0x09,
	TOUCH_OBJECT_N_Z = 0x0a,
	TOUCH_OBJECT_N_X_WIDTH = 0x0b,
	TOUCH_OBJECT_N_Y_WIDTH = 0x0c,
	TOUCH_NUM_OF_ACTIVE_OBJECTS = 0x18,
};

enum tcm_touch_classification {
	TCM_TOUCH_LIFT,
	TCM_TOUCH_FINGER,
	TCM_TOUCH_GLOVED_FINGER,
};

struct tcm_report_config {
	u8 data[TCM_MAX_REPORT_CONFIG_SIZE];
	u16 loop_end[TCM_MAX_REPORT_CONFIG_SIZE];
	u16 size;
	u8 bits[TOUCH_OBJECT_N_Y_WIDTH + 1];
};

struct tcm_report_object {
	u32 classification;
	u32 x;
	u32 y;
	u32 z;
	u32 width_x;
	u32 width_y;
};

/* Controls have no operand. Every other instruction carries a bit width. */
static int tcm_parse_report_config(struct tcm_report_config *config,
				   const u8 *data, size_t size)
{
	unsigned int pc = 0, loop = 0, fields = 0;
	bool in_loop = false, have_loop = false;
	u8 code, bits;

	if (!size || size > sizeof(config->data))
		return -EINVAL;

	memset(config, 0, sizeof(*config));
	memcpy(config->data, data, size);

	while (pc < size) {
		code = data[pc++];
		switch (code) {
		case TOUCH_END:
			if (in_loop || !have_loop)
				return -EINVAL;
			config->size = pc;
			return 0;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
		case TOUCH_FOREACH_OBJECT:
			if (in_loop)
				return -EINVAL;
			loop = pc - 1;
			fields = 0;
			in_loop = true;
			have_loop = true;
			break;
		case TOUCH_FOREACH_END:
			/* Classification and both coordinates are mandatory. */
			if (!in_loop || fields != 7)
				return -EINVAL;
			config->loop_end[loop] = pc - 1;
			in_loop = false;
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			break;
		default:
			if (pc == size || !data[pc])
				return -EINVAL;
			bits = data[pc++];
			if (code >= TOUCH_OBJECT_N_INDEX &&
			    code <= TOUCH_OBJECT_N_Y_WIDTH) {
				if (!in_loop || bits > 32)
					return -EINVAL;
				if (bits > config->bits[code])
					config->bits[code] = bits;
				if (code == TOUCH_OBJECT_N_CLASSIFICATION)
					fields |= 1;
				else if (code == TOUCH_OBJECT_N_X_POSITION)
					fields |= 2;
				else if (code == TOUCH_OBJECT_N_Y_POSITION)
					fields |= 4;
			} else if (code == TOUCH_NUM_OF_ACTIVE_OBJECTS &&
				   (in_loop || bits > 32)) {
				return -EINVAL;
			}
			break;
		}
	}

	return -EINVAL;
}

/* TCM fields are little-endian bit streams, not necessarily byte aligned. */
static u32 tcm_report_read_bits(const u8 *data, unsigned int offset,
				unsigned int bits)
{
	u32 value = 0;
	unsigned int i;

	for (i = 0; i < bits; i++)
		value |= (u32)((data[(offset + i) / 8] >>
				 ((offset + i) % 8)) & 1) << i;

	return value;
}

static int tcm_report_advance(const struct tcm_report_config *config,
			      unsigned int pc, unsigned int end,
			      unsigned int *offset)
{
	u8 code;

	while (pc < end) {
		code = config->data[pc++];
		if (code == TOUCH_END)
			return 0;
		if (code == TOUCH_PAD_TO_NEXT_BYTE) {
			*offset = ALIGN(*offset, 8);
			continue;
		}
		if (code <= TOUCH_FOREACH_END)
			/* Without an object count, consecutive loops are ambiguous. */
			return -EOPNOTSUPP;
		*offset += config->data[pc++];
	}

	return 0;
}

/* Reserve trailing frame fields when FOREACH_ACTIVE_OBJECT has no count. */
static int tcm_report_loop_has_object(const struct tcm_report_config *config,
				     unsigned int start, unsigned int end,
				     unsigned int offset,
				     unsigned int report_bits, bool *more)
{
	unsigned int next = offset;
	int ret;

	ret = tcm_report_advance(config, start, end, &next);
	if (ret)
		return ret;
	ret = tcm_report_advance(config, end + 1, config->size, &next);
	if (ret)
		return ret;
	if (ALIGN(next, 8) <= report_bits) {
		*more = true;
		return 0;
	}

	next = offset;
	ret = tcm_report_advance(config, end + 1, config->size, &next);
	if (ret)
		return ret;
	if (ALIGN(next, 8) != report_bits)
		return -EMSGSIZE;

	*more = false;
	return 0;
}

/*
 * Decode into a scratch frame first. A malformed/truncated frame must not
 * partially update input slots or turn protocol padding into a phantom touch.
 * Unknown scalar fields are skipped by their advertised width, including
 * gesture/grip data wider than 32 bits.
 */
static int tcm_parse_touch_report(const struct tcm_report_config *config,
				  const u8 *data, size_t size,
				  struct tcm_report_object *objects,
				  unsigned int max_objects)
{
	struct tcm_report_object object = { 0 };
	unsigned int pc = 0, offset = 0, report_bits;
	unsigned int start = 0, end = 0, count = 0, index = 0;
	unsigned int active_objects = 0;
	u32 seen = 0, value;
	bool have_count = false, active_only = false, in_loop = false;
	bool more;
	u8 code, bits;
	int ret;

	if (!config->size || !max_objects || max_objects > TCM_MAX_OBJECTS ||
	    size > TCM_MAX_REPORT_CONFIG_SIZE)
		return -EINVAL;

	memset(objects, 0, sizeof(*objects) * max_objects);
	report_bits = size * 8;

	while (pc < config->size) {
		code = config->data[pc++];
		switch (code) {
		case TOUCH_END:
			return ALIGN(offset, 8) == report_bits ? 0 : -EMSGSIZE;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
		case TOUCH_FOREACH_OBJECT:
			start = pc;
			end = config->loop_end[pc - 1];
			active_only = code == TOUCH_FOREACH_ACTIVE_OBJECT;
			count = 0;
			index = 0;
			memset(&object, 0, sizeof(object));
			if (active_only && have_count) {
				more = active_objects != 0;
			} else if (active_only) {
				ret = tcm_report_loop_has_object(config, start, end,
							 offset, report_bits, &more);
				if (ret)
					return ret;
			} else {
				more = true;
			}
			in_loop = more;
			if (!more)
				pc = end + 1;
			break;
		case TOUCH_FOREACH_END:
			if (!in_loop || index >= max_objects)
				return -ERANGE;
			if (seen & (1U << index))
				return -EINVAL;
			seen |= 1U << index;
			objects[index] = object;
			count++;
			if (active_only && !have_count) {
				ret = tcm_report_loop_has_object(config, start, end,
							 offset, report_bits, &more);
				if (ret)
					return ret;
			} else {
				more = count < (active_only ? active_objects : max_objects);
			}
			if (more && count >= max_objects)
				return -EOVERFLOW;
			in_loop = more;
			if (more) {
				pc = start;
				index = count;
				memset(&object, 0, sizeof(object));
			}
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			offset = ALIGN(offset, 8);
			if (offset > report_bits)
				return -EMSGSIZE;
			break;
		default:
			bits = config->data[pc++];
			if (offset > report_bits || bits > report_bits - offset)
				return -EMSGSIZE;

			/* Unused fields can be arbitrarily wide; do not extract them. */
			if ((code < TOUCH_OBJECT_N_INDEX ||
			     code > TOUCH_OBJECT_N_Y_WIDTH) &&
			    code != TOUCH_NUM_OF_ACTIVE_OBJECTS) {
				offset += bits;
				break;
			}

			value = tcm_report_read_bits(data, offset, bits);
			offset += bits;
			switch (code) {
			case TOUCH_OBJECT_N_INDEX:
				index = value;
				break;
			case TOUCH_OBJECT_N_CLASSIFICATION:
				object.classification = value;
				break;
			case TOUCH_OBJECT_N_X_POSITION:
				object.x = value;
				break;
			case TOUCH_OBJECT_N_Y_POSITION:
				object.y = value;
				break;
			case TOUCH_OBJECT_N_Z:
				object.z = value;
				break;
			case TOUCH_OBJECT_N_X_WIDTH:
				object.width_x = value;
				break;
			case TOUCH_OBJECT_N_Y_WIDTH:
				object.width_y = value;
				break;
			case TOUCH_NUM_OF_ACTIVE_OBJECTS:
				if (value > max_objects)
					return -ERANGE;
				active_objects = value;
				have_count = true;
				break;
			}
			break;
		}
	}

	return -EINVAL;
}

#endif /* _SYNAPTICS_TCM_REPORT_H */
