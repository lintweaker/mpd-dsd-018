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

#ifndef MPD_PLAYLIST_HXX
#define MPD_PLAYLIST_HXX

#include "Queue.hxx"
#include "PlaylistError.hxx"

struct PlayerControl;
struct Song;

struct playlist {
	/**
	 * The song queue - it contains the "real" playlist.
	 */
	struct Queue queue;

	/**
	 * This value is true if the player is currently playing (or
	 * should be playing).
	 */
	bool playing;

	/**
	 * If true, then any error is fatal; if false, MPD will
	 * attempt to play the next song on non-fatal errors.  During
	 * seeking, this flag is set.
	 */
	bool stop_on_error;

	/**
	 * If true, then a bulk edit has been initiated by
	 * BeginBulk(), and UpdateQueuedSong() and OnModified() will
	 * be postponed until CommitBulk()
	 */
	bool bulk_edit;

	/**
	 * Has the queue been modified during bulk edit mode?
	 */
	bool bulk_modified;

	/**
	 * Number of errors since playback was started.  If this
	 * number exceeds the length of the playlist, MPD gives up,
	 * because all songs have been tried.
	 */
	unsigned error_count;

	/**
	 * The "current song pointer".  This is the song which is
	 * played when we get the "play" command.  It is also the song
	 * which is currently being played.
	 */
	int current;

	/**
	 * The "next" song to be played, when the current one
	 * finishes.  The decoder thread may start decoding and
	 * buffering it, while the "current" song is still playing.
	 *
	 * This variable is only valid if #playing is true.
	 */
	int queued;

	playlist(unsigned max_length)
		:queue(max_length), playing(false),
		 bulk_edit(false),
		 current(-1), queued(-1) {
	}

	~playlist() {
	}

	uint32_t GetVersion() const {
		return queue.version;
	}

	unsigned GetLength() const {
		return queue.GetLength();
	}

	unsigned PositionToId(unsigned position) const {
		return queue.PositionToId(position);
	}

	gcc_pure
	int GetCurrentPosition() const;

	gcc_pure
	int GetNextPosition() const;

	/**
	 * Returns the song object which is currently queued.  Returns
	 * none if there is none (yet?) or if MPD isn't playing.
	 */
	gcc_pure
	const Song *GetQueuedSong() const;

	/**
	 * This is the "PLAYLIST" event handler.  It is invoked by the
	 * player thread whenever it requests a new queued song, or
	 * when it exits.
	 */
	void SyncWithPlayer(PlayerControl &pc);

protected:
	/**
	 * Called by all editing methods after a modification.
	 * Updates the queue version and emits #IDLE_PLAYLIST.
	 */
	void OnModified();

	/**
	 * Updates the "queued song".  Calculates the next song
	 * according to the current one (if MPD isn't playing, it
	 * takes the first song), and queues this song.  Clears the
	 * old queued song if there was one.
	 *
	 * @param prev the song which was previously queued, as
	 * determined by playlist_get_queued_song()
	 */
	void UpdateQueuedSong(PlayerControl &pc, const Song *prev);

public:
	void BeginBulk();
	void CommitBulk(PlayerControl &pc);

	void Clear(PlayerControl &pc);

	/**
	 * A tag in the play queue has been modified by the player
	 * thread.  Apply the given song's tag to the current song if
	 * the song matches.
	 */
	void TagModified(Song &&song);

	/**
	 * The database has been modified.  Pull all updates.
	 */
	void DatabaseModified();

	PlaylistResult AppendSong(PlayerControl &pc,
				  Song *song,
				  unsigned *added_id=nullptr);

	/**
	 * Appends a local file (outside the music database) to the
	 * playlist.
	 *
	 * Note: the caller is responsible for checking permissions.
	 */
	PlaylistResult AppendFile(PlayerControl &pc,
				  const char *path_utf8,
				  unsigned *added_id=nullptr);

	PlaylistResult AppendURI(PlayerControl &pc,
				 const char *uri_utf8,
				 unsigned *added_id=nullptr);

protected:
	void DeleteInternal(PlayerControl &pc,
			    unsigned song, const Song **queued_p);

public:
	PlaylistResult DeletePosition(PlayerControl &pc,
				      unsigned position);

	PlaylistResult DeleteOrder(PlayerControl &pc,
				   unsigned order) {
		return DeletePosition(pc, queue.OrderToPosition(order));
	}

	PlaylistResult DeleteId(PlayerControl &pc, unsigned id);

	/**
	 * Deletes a range of songs from the playlist.
	 *
	 * @param start the position of the first song to delete
	 * @param end the position after the last song to delete
	 */
	PlaylistResult DeleteRange(PlayerControl &pc,
				   unsigned start, unsigned end);

	void DeleteSong(PlayerControl &pc, const Song &song);

	void Shuffle(PlayerControl &pc, unsigned start, unsigned end);

	PlaylistResult MoveRange(PlayerControl &pc,
				 unsigned start, unsigned end, int to);

	PlaylistResult MoveId(PlayerControl &pc, unsigned id, int to);

	PlaylistResult SwapPositions(PlayerControl &pc,
				     unsigned song1, unsigned song2);

	PlaylistResult SwapIds(PlayerControl &pc,
			       unsigned id1, unsigned id2);

	PlaylistResult SetPriorityRange(PlayerControl &pc,
					unsigned start_position,
					unsigned end_position,
					uint8_t priority);

	PlaylistResult SetPriorityId(PlayerControl &pc,
				     unsigned song_id, uint8_t priority);

	void Stop(PlayerControl &pc);

	PlaylistResult PlayPosition(PlayerControl &pc, int position);

	void PlayOrder(PlayerControl &pc, int order);

	PlaylistResult PlayId(PlayerControl &pc, int id);

	void PlayNext(PlayerControl &pc);

	void PlayPrevious(PlayerControl &pc);

	PlaylistResult SeekSongOrder(PlayerControl &pc,
				     unsigned song_order,
				     float seek_time);

	PlaylistResult SeekSongPosition(PlayerControl &pc,
					unsigned song_position,
					float seek_time);

	PlaylistResult SeekSongId(PlayerControl &pc,
				  unsigned song_id, float seek_time);

	/**
	 * Seek within the current song.  Fails if MPD is not currently
	 * playing.
	 *
	 * @param time the time in seconds
	 * @param relative if true, then the specified time is relative to the
	 * current position
	 */
	PlaylistResult SeekCurrent(PlayerControl &pc,
				   float seek_time, bool relative);

	bool GetRepeat() const {
		return queue.repeat;
	}

	void SetRepeat(PlayerControl &pc, bool new_value);

	bool GetRandom() const {
		return queue.random;
	}

	void SetRandom(PlayerControl &pc, bool new_value);

	bool GetSingle() const {
		return queue.single;
	}

	void SetSingle(PlayerControl &pc, bool new_value);

	bool GetConsume() const {
		return queue.consume;
	}

	void SetConsume(bool new_value);
};

#endif
