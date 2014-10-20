/*
 * Copyright (C) 2014 Jurgen Kramer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "PcmDsdNative.hxx"
#include "PcmBuffer.hxx"
#include "AudioFormat.hxx"

/* JK For debug info */
//#include "util/Domain.hxx"
//#include "Log.hxx"

//static constexpr Domain dsdn_dom("dsd_native");

constexpr
static inline uint32_t
pcm_two_dsd_native(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return 0x00000000 | (a << 24 ) | (b << 16) | (c << 8) | d;
}

const uint32_t *
pcm_dsd_native(PcmBuffer &buffer, unsigned channels,
	       const uint8_t *src, size_t src_size,
	       size_t *dest_size_r)
{
	assert(audio_valid_channel_count(channels));
	assert(src != NULL);
	assert(src_size > 0);
	assert(src_size % channels == 0);

	const unsigned num_src_samples = src_size;
	const unsigned num_src_frames = num_src_samples / channels;

	const unsigned num_frames = num_src_frames / 2;
	//const unsigned num_samples = num_frames * channels;

	const size_t dest_size = src_size;

	*dest_size_r = dest_size;
	uint32_t *const dest0 = (uint32_t *)buffer.Get(dest_size),
		*dest = dest0;

	for (unsigned i = num_frames / 2; i > 0 ; --i) {

		/* Left channel */
		*dest++ = pcm_two_dsd_native(src[6], src[4], src[2], src[0]);
		/* Right channel */
		*dest++ = pcm_two_dsd_native(src[7], src[5], src[3], src[1]);

		src += 8;
	}

	return dest0;
}
