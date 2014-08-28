/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
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

/* \file
 *
 * This plugin decodes DSD hybrid data (SACD) embedded in M4A files.
 *
 * All functions common to both DSD decoders have been moved to dsdlib
 */

#include "config.h"
#include "DsdHybridDecoderPlugin.hxx"
#include "DecoderAPI.hxx"
#include "InputStream.hxx"
#include "CheckAudioFormat.hxx"
#include "util/Error.hxx"
#include "system/ByteOrder.hxx"
#include "DsdLib.hxx"
#include "tag/TagHandler.hxx"
#include "Log.hxx"
#include "util/Domain.hxx"

#include <unistd.h>
#include <stdio.h> /* for SEEK_SET, SEEK_CUR */
#include <string.h>

#define BUFFER_SIZE 1024

static constexpr Domain dsdhybrid_domain("dsdhybrid");

struct DsdHybridMetaData {
	unsigned sample_rate, channels;
	/* Data atom offset and size */
	InputStream::offset_type data_offset;
	uint64_t chunk_size;
	/* "moov" atom offset and size (for tags processing) */
	InputStream::offset_type moov_offset;
	uint64_t moov_size;
};

static inline uint32_t
dsdhybrid_read_be32_size(uint8_t *ptr)
{
	return FromBE32(*(uint32_t *)ptr);
}

/**
 * Read and parse all needed atoms for DSD hybrid files.
 */
static bool
dsdhybrid_read_metadata(Decoder *decoder, InputStream &is,
			DsdHybridMetaData *metadata)
{
	uint8_t buffer[8];
	uint8_t all_atoms_found;
	uint32_t atom_size;
	uint32_t dsd_version, dsd_channels, dsd_samplefreq, dsd_format;
	uint32_t data_offset, chunk_size, moov_offset, moov_size;

	/* Read first atom header and detect file type. */
	if (!decoder_read_full(decoder, is, buffer, 4))
		return false;
	atom_size = dsdhybrid_read_be32_size(buffer);
	if (!decoder_read_full(decoder, is, buffer, 8))
		return false;
	if (memcmp(buffer, "ftypM4A ", 8) != 0)
		return false;

	/* Skip to next atom. */
	dsdlib_skip(decoder, is, atom_size - 12);

	/* Process all atoms in file */
	all_atoms_found = 0;
	while (decoder_read_full(decoder, is, buffer, 4)) /* Read atom size */
	{
		/* Atom body size */
		atom_size = dsdhybrid_read_be32_size(buffer) - 8;

		/* Read atom name */
		if (!decoder_read_full(decoder, is, buffer, 4))
			return false;

		/* "bphv" atom stores DSD version */
		if (memcmp(buffer, "bphv", 4) == 0)
		{
			if (!decoder_read_full(decoder, is, buffer, 4))
				return false;
			dsd_version = dsdhybrid_read_be32_size(buffer);
			dsdlib_skip(decoder, is, atom_size - 4);
			all_atoms_found |= 1;
			continue;
		}

		/* "bphc" atom stores channels */
		if (memcmp(buffer, "bphc", 4) == 0)
		{
			if (!decoder_read_full(decoder, is, buffer, 4))
				return false;
			dsd_channels = dsdhybrid_read_be32_size(buffer);
			dsdlib_skip(decoder, is, atom_size - 4);
			all_atoms_found |= 2;
			continue;
		}

		/* "bphr" atom stores sample frequency */
		if (memcmp(buffer, "bphr", 4) == 0)
		{
			if (!decoder_read_full(decoder, is, buffer, 4))
				return false;
			dsd_samplefreq = dsdhybrid_read_be32_size(buffer);
			dsdlib_skip(decoder, is, atom_size - 4);
			all_atoms_found |= 4;
			continue;
		}

		/* "bphf" atom stores DSD format */
		if (memcmp(buffer, "bphf", 4) == 0)
		{
			if (!decoder_read_full(decoder, is, buffer, 4))
				return false;
			dsd_format = dsdhybrid_read_be32_size(buffer);
			dsdlib_skip(decoder, is, atom_size - 4);
			all_atoms_found |= 8;
			continue;
		}

		/* "bphd" atom stores DSD audio in DoP-ready form */
		if (memcmp(buffer, "bphd", 4) == 0)
		{
			data_offset = is.GetOffset();
			chunk_size = (uint64_t)atom_size;
			all_atoms_found |= 16;
		}

		/* "moov" atom stores metadata */
		if (memcmp(buffer, "moov", 4) == 0)
		{
			moov_offset = is.GetOffset();
			moov_size = (uint64_t)atom_size;
			all_atoms_found |= 32;
		}

		dsdlib_skip(decoder, is, atom_size);
	}

	/* All atoms found? */
	if (all_atoms_found != 63)
		return false;

	/* for now, only support version 1 of the standard, DSD raw stereo
	   files with a sample freq of 2822400 or 5644800 Hz*/
	if (dsd_version != 1 || dsd_format != 0
	    || dsd_channels != 2
	    || (!dsdlib_valid_freq(dsd_samplefreq)))
		return false;

	/* data_size cannot be bigger or equal to total file size */
	const uint64_t size = (uint64_t)is.GetSize();
	if (chunk_size >= size)
		return false;

	metadata->sample_rate = dsd_samplefreq;
	metadata->channels    = dsd_channels;
	metadata->data_offset = data_offset;
	metadata->chunk_size  = chunk_size;
	metadata->moov_offset = moov_offset;
	metadata->moov_size   = moov_size;

	return true;
}

static bool
dsdhybrid_tags_find_child_atom(uint8_t **atom_ptr, size_t *atom_size, const char *atom_name)
{
	uint8_t *current_atom_ptr = *atom_ptr;
	uint8_t *current_atom_name_ptr;
	uint32_t current_atom_size;
	size_t atom_size_remain = *atom_size;

	if (*atom_size == 0)
		return false;

	while (atom_size_remain > 0)
	{
		current_atom_size = dsdhybrid_read_be32_size(current_atom_ptr) - 8;
		if (current_atom_size > (atom_size_remain - 8))
			return false;
		current_atom_ptr += 4;

		current_atom_name_ptr = current_atom_ptr;
		current_atom_ptr += 4;

		if (memcmp(current_atom_name_ptr, atom_name, 4) == 0)
		{
			*atom_ptr  = current_atom_ptr;
			*atom_size = current_atom_size;
			return true;
		}
		current_atom_ptr += current_atom_size;
		atom_size_remain -= current_atom_size + 8;
	}

	return false;
}

static bool
dsdhybrid_get_raw_tag(uint8_t **raw_tag_ptr, size_t *raw_tag_size)
{
	uint8_t *data_ptr = *raw_tag_ptr;
	uint32_t data_size = dsdhybrid_read_be32_size(data_ptr);

	if (*raw_tag_size != data_size)
		return false;

	data_ptr += 4;

	if (memcmp(data_ptr, "data", 4) != 0)
		return false;

	data_ptr  += 12;
	data_size -= 16;

	*raw_tag_ptr  = data_ptr;
	*raw_tag_size = data_size;

	return true;
}

static void
dsdhybrid_invoke_text_tag(uint8_t *tag_data_ptr,
			  size_t tag_data_size,
			  enum TagType tag_type,
			  const struct tag_handler *handler,
			  void *handler_ctx)
{
	void *string_buffer;

	if (!dsdhybrid_get_raw_tag(&tag_data_ptr, &tag_data_size))
		return;

	string_buffer = malloc(tag_data_size + 1);
	if (string_buffer == nullptr)
		return;

	memset(string_buffer, 0, tag_data_size + 1);
	memcpy(string_buffer, tag_data_ptr, tag_data_size);

	tag_handler_invoke_tag(handler, handler_ctx, tag_type,
				(char *)string_buffer);

	free(string_buffer);
}

static void
dsdhybrid_invoke_track_disc_tag(uint8_t *tag_data_ptr,
				size_t tag_data_size,
				enum TagType tag_type,
				const struct tag_handler *handler,
				void *handler_ctx)
{
	char *string_buffer;
	uint8_t num, total_num;

	if (!dsdhybrid_get_raw_tag(&tag_data_ptr, &tag_data_size))
		return;

	tag_data_ptr += 3;
	num = *tag_data_ptr;

	tag_data_ptr += 2;
	total_num = *tag_data_ptr;

	int ret = asprintf(&string_buffer, "%u/%u", num, total_num);
	if (string_buffer == nullptr || ret == 0)
		return;

	tag_handler_invoke_tag(handler, handler_ctx, tag_type, string_buffer);

	free(string_buffer);
}

static void
dsdhybrid_invoke_genre_num_tag(uint8_t *tag_data_ptr,
			       size_t tag_data_size,
			       enum TagType tag_type,
			       const struct tag_handler *handler,
			       void *handler_ctx)
{
	uint8_t num;
	static const char *genres[] =
	{
		"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
		"Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
		"Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
		"Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
		"Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
		"Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
		"Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
		"Alt. Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
		"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
		"Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
		"Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
		"Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
		"Native American", "Cabaret", "New Wave", "Psychadelic",
		"Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
		"Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical",
		"Rock & Roll", "Hard Rock", "Folk", "Folk/Rock",
		"National Folk", "Swing", "Fusion", "Bebob", "Latin",
		"Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
		"Progressive Rock", "Psychadelic Rock", "Symphonic Rock",
		"Slow Rock", "Big Band", "Chorus", "Easy Listening",
		"Acoustic", "Humour", "Speech", "Chanson", "Opera",
		"Chamber Music", "Sonata", "Symphony", "Booty Bass",
		"Primus", "Porn Groove", "Satire", "Slow Jam", "Club",
		"Tango", "Samba", "Folklore", "Ballad", "Power Ballad",
		"Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
		"Drum Solo", "A Capella", "Euro-House", "Dance Hall",
		"Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
		"Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat",
		"Christian Gangsta Rap", "Heavy Metal", "Black Metal",
		"Crossover", "Contemporary Christian", "Christian Rock",
		"Merengue", "Salsa", "Thrash Metal", "Anime", "Jpop",
		"Synthpop"
	};

	if (!dsdhybrid_get_raw_tag(&tag_data_ptr, &tag_data_size))
		return;

	tag_data_ptr += 1;
	num = *tag_data_ptr;

	if (num > 148)
		return;

	tag_handler_invoke_tag(handler, handler_ctx, tag_type, genres[num - 1]);
}

static void
dsdhybrid_read_tags_from_buffer(uint8_t *buffer,
				size_t buffer_size,
				const struct tag_handler *handler,
				void *handler_ctx)
{
	uint8_t *atom_ptr, *list_entry_name_ptr;
	size_t atom_size, list_entry_size;
	
	/* Well known tags */
	const unsigned char tag_artist      [] = { 0xa9, 'A', 'R', 'T' };
	const unsigned char tag_album       [] = { 0xa9, 'a', 'l', 'b' };
	const unsigned char tag_album_artist[] = {  'a', 'A', 'R', 'T' };
	const unsigned char tag_title       [] = { 0xa9, 'n', 'a', 'm' };
	const unsigned char tag_track       [] = {  't', 'r', 'k', 'n' };
	const unsigned char tag_genre       [] = { 0xa9, 'g', 'e', 'n' };
	const unsigned char tag_genre_num   [] = {  'g', 'n', 'r', 'e' };
	const unsigned char tag_date        [] = { 0xa9, 'd', 'a', 'y' };
	const unsigned char tag_composer    [] = { 0xa9, 'w', 'r', 't' };
	const unsigned char tag_comment     [] = { 0xa9, 'c', 'm', 't' };
	const unsigned char tag_disc        [] = {  'd', 'i', 's', 'k' };

	/* Now we are at "moov" atom */
	atom_ptr  = buffer;
	atom_size = buffer_size;

	/* Find "moov.udta.meta" atom */
	if (!dsdhybrid_tags_find_child_atom(&atom_ptr, &atom_size, "udta") ||
	    !dsdhybrid_tags_find_child_atom(&atom_ptr, &atom_size, "meta"))
		return;

	/* Skip 4 bytes flags of "meta" atom */
	atom_ptr  += 4;
	atom_size -= 4;

	/* Find "moov.udta.meta.ilst" atom */
	if (!dsdhybrid_tags_find_child_atom(&atom_ptr, &atom_size, "ilst"))
		return;

	/* Scan "moov.udta.meta.ilst" atom entries */
	while (atom_size > 0)
	{
		list_entry_size = dsdhybrid_read_be32_size(atom_ptr) - 8;
		if (list_entry_size > (atom_size - 8))
			return;
		atom_ptr += 4;

		list_entry_name_ptr = atom_ptr;
		atom_ptr += 4;

		if (memcmp(list_entry_name_ptr, tag_artist, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_ARTIST, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_album, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_ALBUM, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_album_artist, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_ALBUM_ARTIST, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_title, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_TITLE, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_genre, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_GENRE, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_date, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_DATE, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_composer, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_COMPOSER, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_comment, 4) == 0)
			dsdhybrid_invoke_text_tag(atom_ptr, list_entry_size,
							TAG_COMMENT, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_track, 4) == 0)
			dsdhybrid_invoke_track_disc_tag(atom_ptr, list_entry_size,
							TAG_TRACK, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_disc, 4) == 0)
			dsdhybrid_invoke_track_disc_tag(atom_ptr, list_entry_size,
							TAG_DISC, handler, handler_ctx);

		else if (memcmp(list_entry_name_ptr, tag_genre_num, 4) == 0)
			dsdhybrid_invoke_genre_num_tag(atom_ptr, list_entry_size,
							TAG_GENRE, handler, handler_ctx);

		atom_ptr  += list_entry_size;
		atom_size -= list_entry_size + 8;
	}

}

static void
dsdhybrid_read_tags(InputStream &is,
			const struct tag_handler *handler,
			void *handler_ctx,
			uint64_t moov_offset,
			uint64_t moov_size)
{
	Error error;
	uint8_t *buffer;

	if (!is.LockSeek(moov_offset, SEEK_SET, error))
		return;

	buffer = (uint8_t *)malloc(moov_size);
	if (buffer == nullptr)
		return;
	
	if (decoder_read_full(nullptr, is, buffer, moov_size))
		dsdhybrid_read_tags_from_buffer(buffer,
						(size_t)moov_size,
						handler,
						handler_ctx);

	free(buffer);
}

static bool
dsdhybrid_decode_chunk(Decoder &decoder, InputStream &is,
			unsigned channels,
			uint64_t chunk_size,
			InputStream::offset_type stream_start_offset,
			unsigned sample_rate)
{
	uint8_t buffer[BUFFER_SIZE];
	const size_t block_size = BUFFER_SIZE;
	const size_t buffer_size = BUFFER_SIZE;
	Error error;
	const uint64_t stream_end_offset = chunk_size + (uint64_t) stream_start_offset;

	if (!is.LockSeek(stream_start_offset, SEEK_SET, error))
		return false;

	while (chunk_size > 0) {
		/* see how much aligned data from the remaining chunk
		   fits into the local buffer */
		size_t now_size = buffer_size;
		if (chunk_size < (uint64_t)now_size)
			now_size = (unsigned)chunk_size;

		if (!decoder_read_full(&decoder, is, buffer, now_size))
			return false;

		const size_t nbytes = now_size;
		chunk_size -= nbytes;

		const auto cmd = decoder_data(decoder, is, buffer, nbytes, sample_rate / 1000);
		switch (cmd) {
		case DecoderCommand::NONE:
			break;

		case DecoderCommand::START:
		case DecoderCommand::STOP:
			return false;

		case DecoderCommand::SEEK:

			InputStream::offset_type offset;
			InputStream::offset_type curpos = is.GetOffset();
			offset = (InputStream::offset_type) (stream_start_offset +
				 (channels * (sample_rate / 8) * decoder_seek_where(decoder)));

			if (offset < stream_start_offset)
				offset = stream_start_offset;

			if ((unsigned) offset > stream_end_offset)
				offset = stream_end_offset;

			/* Round new offset to the nearest data block */
			if ( offset > stream_start_offset) {
				offset -= stream_start_offset;
				float i = (float) offset / (float) block_size;
				unsigned t = (unsigned) i;
				if ( t % 2 == 1 ) {
					t -= 1;
				}
				offset = ( t * block_size) + stream_start_offset;
			}

			if (offset < curpos)
				chunk_size = chunk_size + (curpos - offset);

			if (offset > curpos)
				chunk_size = chunk_size - (offset - curpos);

			if  (is.LockSeek(offset, SEEK_SET, error)) {
				decoder_command_finished(decoder);
			} else {
				LogError(error);
				decoder_seek_error(decoder);
				break;
			}
		}
	}
	return dsdlib_skip(&decoder, is, chunk_size);
}

static void
dsdhybrid_stream_decode(Decoder &decoder, InputStream &is)
{
	/* check if it is a proper DSD hybrid file */
	DsdHybridMetaData metadata;
	if (!dsdhybrid_read_metadata(&decoder, is, &metadata))
		return;

	Error error;
	AudioFormat audio_format;
	if (!audio_format_init_checked(audio_format, metadata.sample_rate / 8,
				       SampleFormat::DSD,
				       metadata.channels, error)) {
		LogError(error);
		return;
	}
	/* Calculate song time from DSD chunk size and sample frequency */
	uint64_t chunk_size = metadata.chunk_size;
	float songtime = ((chunk_size / metadata.channels) * 8) /
			 (float) metadata.sample_rate;

	/* success: file was recognized */
	decoder_initialized(decoder, audio_format, true, songtime);

	if (!dsdhybrid_decode_chunk(decoder, is,
				    metadata.channels,
				    chunk_size,
				    metadata.data_offset,
				    metadata.sample_rate))
		return;
}

static bool
dsdhybrid_scan_stream(InputStream &is,
		      gcc_unused const struct tag_handler *handler,
		      gcc_unused void *handler_ctx)
{
	/* check DSD hybrid metadata */
	DsdHybridMetaData metadata;
	if (!dsdhybrid_read_metadata(nullptr, is, &metadata))
		return false;

	AudioFormat audio_format;
	if (!audio_format_init_checked(audio_format, metadata.sample_rate / 8,
				       SampleFormat::DSD,
				       metadata.channels, IgnoreError()))
		/* refuse to parse files which we cannot play anyway */
		return false;

	/* calculate song time and add as tag */
	unsigned songtime = ((metadata.chunk_size / metadata.channels) * 8) /
			    metadata.sample_rate;
	tag_handler_invoke_duration(handler, handler_ctx, songtime);

	/* Tags processing */
	dsdhybrid_read_tags(is, handler, handler_ctx,
			    metadata.moov_offset, metadata.moov_size);

	return true;
}

static const char *const dsdhybrid_suffixes[] = {
	"m4a",
	nullptr
};

static const char *const dsdhybrid_mime_types[] = {
	"application/m4a",
	nullptr
};

const struct DecoderPlugin dsdhybrid_decoder_plugin = {
	"dsdhybrid",
	nullptr,
	nullptr,
	dsdhybrid_stream_decode,
	nullptr,
	nullptr,
	dsdhybrid_scan_stream,
	nullptr,
	dsdhybrid_suffixes,
	dsdhybrid_mime_types,
};
