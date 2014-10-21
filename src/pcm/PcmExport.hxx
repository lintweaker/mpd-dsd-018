/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
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

#ifndef PCM_EXPORT_HXX
#define PCM_EXPORT_HXX

#include "check.h"
#include "PcmBuffer.hxx"
#include "AudioFormat.hxx"

struct AudioFormat;

/**
 * An object that handles export of PCM samples to some instance
 * outside of MPD.  It has a few more options to tweak the binary
 * representation which are not supported by the pcm_convert library.
 */
struct PcmExport {
	/**
	 * The buffer is used to convert DSD samples to the
	 * DSD-over-USB format.
	 *
	 * @see #dsd_usb
	 */
	PcmBuffer dsd_buffer;

	/**
	 * The buffer is used to pack samples, removing padding.
	 *
	 * @see #pack24
	 */
	PcmBuffer pack_buffer;

	/**
	 * The buffer is used to reverse the byte order.
	 *
	 * @see #reverse_endian
	 */
	PcmBuffer reverse_buffer;

	/**
	 * The number of channels.
	 */
	uint8_t channels;

	/**
	 * Convert DSD to DSD-over-USB?  Input format must be
	 * SampleFormat::DSD and output format must be
	 * SampleFormat::S24_P32.
	 */
	bool dsd_usb;

	/**
	 * Output native DSD?
	 * dsd_native_type contains requested output type
	 */
	bool dsd_native;

	/**
	 * DSD native output type
	 * 0 = DSD_U8, no export needed
	 * 1 = DSD_U16_LE, reserved. Not supported yet
	 * 2 = DSD_U32_LE, e.g. XMOS based USB DACs
	 */
	unsigned dsd_native_type;

	/**
	 * Convert (padded) 24 bit samples to 32 bit by shifting 8
	 * bits to the left?
	 */
	bool shift8;

	/**
	 * Pack 24 bit samples?
	 */
	bool pack24;

	/**
	 * Export the samples in reverse byte order?  A non-zero value
	 * means the option is enabled and represents the size of each
	 * sample (2 or bigger).
	 */
	uint8_t reverse_endian;

	/**
	 * Open the #pcm_export_state object.
	 *
	 * There is no "close" method.  This function may be called multiple
	 * times to reuse the object, until pcm_export_deinit() is called.
	 *
	 * This function cannot fail.
	 *
	 * @param channels the number of channels; ignored unless dsd_usb is set
	 */
	void Open(SampleFormat sample_format, unsigned channels,
		  bool dsd_usb, bool shift8, bool pack, bool reverse_endian,
		  bool dsd_native, unsigned dsd_native_type);

	/**
	 * Calculate the size of one output frame.
	 */
	gcc_pure
	size_t GetFrameSize(const AudioFormat &audio_format) const;

	/**
	 * Export a PCM buffer.
	 *
	 * @param state an initialized and open pcm_export_state object
	 * @param src the source PCM buffer
	 * @param src_size the size of #src in bytes
	 * @param dest_size_r returns the number of bytes of the destination buffer
	 * @return the destination buffer (may be a pointer to the source buffer)
	 */
	const void *Export(const void *src, size_t src_size,
			   size_t &dest_size_r);

	/**
	 * Converts the number of consumed bytes from the pcm_export()
	 * destination buffer to the according number of bytes from the
	 * pcm_export() source buffer.
	 */
	gcc_pure
	size_t CalcSourceSize(size_t dest_size) const;
};

#endif
