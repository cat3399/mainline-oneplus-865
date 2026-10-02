/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MXM1120_CODEC_H
#define _MXM1120_CODEC_H

static inline int mxm1120_decode(unsigned char low, unsigned char high)
{
	int value = low | ((high & 0x40) << 2);

	return high & 0x80 ? value - 512 : value;
}

static inline int ist8801_decode(unsigned char low, unsigned char high)
{
	int value = low | (high << 8);

	return high & 0x80 ? value - 65536 : value;
}

#endif /* _MXM1120_CODEC_H */
