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

#include "config.h"
#include "CommandLine.hxx"
#include "ls.hxx"
#include "LogInit.hxx"
#include "Log.hxx"
#include "ConfigGlobal.hxx"
#include "DecoderList.hxx"
#include "DecoderPlugin.hxx"
#include "OutputList.hxx"
#include "OutputPlugin.hxx"
#include "InputRegistry.hxx"
#include "InputPlugin.hxx"
#include "PlaylistRegistry.hxx"
#include "PlaylistPlugin.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/Traits.hxx"
#include "fs/FileSystem.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "system/FatalError.hxx"

#ifdef ENABLE_ENCODER
#include "EncoderList.hxx"
#include "EncoderPlugin.hxx"
#endif

#ifdef ENABLE_ARCHIVE
#include "ArchiveList.hxx"
#include "ArchivePlugin.hxx"
#endif

#include <glib.h>

#include <stdio.h>
#include <stdlib.h>

#ifdef WIN32
#define CONFIG_FILE_LOCATION		"\\mpd\\mpd.conf"
#else
#define USER_CONFIG_FILE_LOCATION1	".mpdconf"
#define USER_CONFIG_FILE_LOCATION2	".mpd/mpd.conf"
#define USER_CONFIG_FILE_LOCATION_XDG	"mpd/mpd.conf"
#endif

static constexpr Domain cmdline_domain("cmdline");

gcc_noreturn
static void version(void)
{
	puts("Music Player Daemon " VERSION "\n"
	     "\n"
	     "Copyright (C) 2003-2007 Warren Dukes <warren.dukes@gmail.com>\n"
	     "Copyright (C) 2008-2014 Max Kellermann <max@duempel.org>\n"
	     "This is free software; see the source for copying conditions.  There is NO\n"
	     "warranty; not even MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
	     "\n"
	     "Decoders plugins:");

	decoder_plugins_for_each([](const DecoderPlugin &plugin){
			printf(" [%s]", plugin.name);

			const char *const*suffixes = plugin.suffixes;
			if (suffixes != nullptr)
				for (; *suffixes != nullptr; ++suffixes)
					printf(" %s", *suffixes);

			puts("");
		});

	puts("\n"
	     "Output plugins:");
	audio_output_plugins_for_each(plugin)
		printf(" %s", plugin->name);
	puts("");

#ifdef ENABLE_ENCODER
	puts("\n"
	     "Encoder plugins:");
	encoder_plugins_for_each(plugin)
		printf(" %s", plugin->name);
	puts("");
#endif

#ifdef ENABLE_ARCHIVE
	puts("\n"
	     "Archive plugins:");
	archive_plugins_for_each(plugin) {
		printf(" [%s]", plugin->name);

		const char *const*suffixes = plugin->suffixes;
		if (suffixes != nullptr)
			for (; *suffixes != nullptr; ++suffixes)
				printf(" %s", *suffixes);

		puts("");
	}
#endif

	puts("\n"
	     "Input plugins:");
	input_plugins_for_each(plugin)
		printf(" %s", plugin->name);

	puts("\n\n"
	     "Playlist plugins:");
	playlist_plugins_for_each(plugin)
		printf(" %s", plugin->name);

	puts("\n\n"
	     "Protocols:");
	print_supported_uri_schemes_to_fp(stdout);

	exit(EXIT_SUCCESS);
}

static const char *summary =
	"Music Player Daemon - a daemon for playing music.";

gcc_pure
static AllocatedPath
PathBuildChecked(const AllocatedPath &a, PathTraits::const_pointer b)
{
	if (a.IsNull())
		return AllocatedPath::Null();

	return AllocatedPath::Build(a, b);
}

bool
parse_cmdline(int argc, char **argv, struct options *options,
	      Error &error)
{
	GOptionContext *context;
	bool ret;
	static gboolean option_version,
		option_no_daemon,
		option_no_config;
	const GOptionEntry entries[] = {
		{ "kill", 0, 0, G_OPTION_ARG_NONE, &options->kill,
		  "kill the currently running mpd session", nullptr },
		{ "no-config", 0, 0, G_OPTION_ARG_NONE, &option_no_config,
		  "don't read from config", nullptr },
		{ "no-daemon", 0, 0, G_OPTION_ARG_NONE, &option_no_daemon,
		  "don't detach from console", nullptr },
		{ "stdout", 0, 0, G_OPTION_ARG_NONE, &options->log_stderr,
		  nullptr, nullptr },
		{ "stderr", 0, 0, G_OPTION_ARG_NONE, &options->log_stderr,
		  "print messages to stderr", nullptr },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &options->verbose,
		  "verbose logging", nullptr },
		{ "version", 'V', 0, G_OPTION_ARG_NONE, &option_version,
		  "print version number", nullptr },
		{ nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr }
	};

	options->kill = false;
	options->daemon = true;
	options->log_stderr = false;
	options->verbose = false;

	context = g_option_context_new("[path/to/mpd.conf]");
	g_option_context_add_main_entries(context, entries, nullptr);

	g_option_context_set_summary(context, summary);

	GError *gerror = nullptr;
	ret = g_option_context_parse(context, &argc, &argv, &gerror);
	g_option_context_free(context);

	if (!ret)
		FatalError("option parsing failed", gerror);

	if (option_version)
		version();

	/* initialize the logging library, so the configuration file
	   parser can use it already */
	log_early_init(options->verbose);

	options->daemon = !option_no_daemon;

	if (option_no_config) {
		LogDebug(cmdline_domain,
			 "Ignoring config, using daemon defaults");
		return true;
	} else if (argc <= 1) {
		/* default configuration file path */

#ifdef WIN32
		AllocatedPath path = PathBuildChecked(AllocatedPath::FromUTF8(g_get_user_config_dir()),
					     CONFIG_FILE_LOCATION);
		if (!path.IsNull() && FileExists(path))
			return ReadConfigFile(path, error);

		const char *const*system_config_dirs =
			g_get_system_config_dirs();

		for (unsigned i = 0; system_config_dirs[i] != nullptr; ++i) {
			path = PathBuildChecked(AllocatedPath::FromUTF8(system_config_dirs[i]),
						CONFIG_FILE_LOCATION);
			if (!path.IsNull() && FileExists(path))
				return ReadConfigFile(path, error);
		}
#else
		AllocatedPath path = PathBuildChecked(AllocatedPath::FromUTF8(g_get_user_config_dir()),
						      USER_CONFIG_FILE_LOCATION_XDG);
		if (!path.IsNull() && FileExists(path))
			return ReadConfigFile(path, error);

		path = PathBuildChecked(AllocatedPath::FromUTF8(g_get_home_dir()),
					     USER_CONFIG_FILE_LOCATION1);
		if (!path.IsNull() && FileExists(path))
			return ReadConfigFile(path, error);

		path = PathBuildChecked(AllocatedPath::FromUTF8(g_get_home_dir()),
					USER_CONFIG_FILE_LOCATION2);
		if (!path.IsNull() && FileExists(path))
			return ReadConfigFile(path, error);

		path = AllocatedPath::FromUTF8(SYSTEM_CONFIG_FILE_LOCATION);
		if (!path.IsNull() && FileExists(path))
			return ReadConfigFile(path, error);
#endif

		error.Set(cmdline_domain, "No configuration file found");
		return false;
	} else if (argc == 2) {
		/* specified configuration file */
		return ReadConfigFile(Path::FromFS(argv[1]), error);
	} else {
		error.Set(cmdline_domain, "too many arguments");
		return false;
	}
}
