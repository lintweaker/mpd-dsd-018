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
#include "ClientFile.hxx"
#include "Client.hxx"
#include "protocol/Ack.hxx"
#include "fs/Path.hxx"
#include "fs/FileSystem.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

bool
client_allow_file(const Client &client, Path path_fs, Error &error)
{
#ifdef WIN32
	(void)client;
	(void)path_fs;

	error.Set(ack_domain, ACK_ERROR_PERMISSION, "Access denied");
	return false;
#else
	const int uid = client.GetUID();
	if (uid >= 0 && (uid_t)uid == geteuid())
		/* always allow access if user runs his own MPD
		   instance */
		return true;

	if (uid < 0) {
		/* unauthenticated client */
		error.Set(ack_domain, ACK_ERROR_PERMISSION, "Access denied");
		return false;
	}

	struct stat st;
	if (!StatFile(path_fs, st)) {
		error.SetErrno();
		return false;
	}

	if (st.st_uid != (uid_t)uid && (st.st_mode & 0444) != 0444) {
		/* client is not owner */
		error.Set(ack_domain, ACK_ERROR_PERMISSION, "Access denied");
		return false;
	}

	return true;
#endif
}
