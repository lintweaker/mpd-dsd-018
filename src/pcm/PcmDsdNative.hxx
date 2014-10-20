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

#ifndef MPD_PCM_DSD_NATIVE_HXX
#define MPD_PCM_DSD_NATIVE_HXX

#include "check.h"

#include <stdint.h>
#include <stddef.h>

class PcmBuffer;

/**
 * Pack DSD 1 bit samples into DSD_U32_LE samples for
 * native DSD playback
 */
const uint32_t *
pcm_dsd_native(PcmBuffer &buffer, unsigned channels,
	       const uint8_t *src, size_t src_size,
	       size_t *dest_size_r);

#endif
