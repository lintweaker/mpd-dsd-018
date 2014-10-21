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

#include "config.h"
#include "DecoderInternal.hxx"
#include "DecoderControl.hxx"
#include "MusicPipe.hxx"
#include "MusicBuffer.hxx"
#include "MusicChunk.hxx"
#include "tag/Tag.hxx"

#include <assert.h>

Decoder::~Decoder()
{
	/* caller must flush the chunk */
	assert(chunk == nullptr);

	delete song_tag;
	delete stream_tag;
	delete decoder_tag;
}

/**
 * All chunks are full of decoded data; wait for the player to free
 * one.
 */
static DecoderCommand
need_chunks(DecoderControl &dc)
{
	if (dc.command == DecoderCommand::NONE)
		dc.Wait();

	return dc.command;
}

struct music_chunk *
decoder_get_chunk(Decoder &decoder)
{
	DecoderControl &dc = decoder.dc;
	DecoderCommand cmd;

	if (decoder.chunk != nullptr)
		return decoder.chunk;

	do {
		decoder.chunk = dc.buffer->Allocate();
		if (decoder.chunk != nullptr) {
			decoder.chunk->replay_gain_serial =
				decoder.replay_gain_serial;
			if (decoder.replay_gain_serial != 0)
				decoder.chunk->replay_gain_info =
					decoder.replay_gain_info;

			return decoder.chunk;
		}

		dc.Lock();
		cmd = need_chunks(dc);
		dc.Unlock();
	} while (cmd == DecoderCommand::NONE);

	return nullptr;
}

void
decoder_flush_chunk(Decoder &decoder)
{
	DecoderControl &dc = decoder.dc;
	assert(!decoder.seeking);
	assert(!decoder.initial_seek_running);
	assert(!decoder.initial_seek_pending);

	assert(decoder.chunk != nullptr);

	if (decoder.chunk->IsEmpty())
		dc.buffer->Return(decoder.chunk);
	else
		dc.pipe->Push(decoder.chunk);

	decoder.chunk = nullptr;

	dc.Lock();
	if (dc.client_is_waiting)
		dc.client_cond.signal();
	dc.Unlock();
}
