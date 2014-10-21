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
#include "AlsaOutputPlugin.hxx"
#include "OutputAPI.hxx"
#include "MixerList.hxx"
#include "pcm/PcmExport.hxx"
#include "util/Manual.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <glib.h>
#include <alsa/asoundlib.h>

#include <string>

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API

static const char default_device[] = "default";

static constexpr unsigned MPD_ALSA_BUFFER_TIME_US = 500000;

#define MPD_ALSA_RETRY_NR 5

typedef snd_pcm_sframes_t alsa_writei_t(snd_pcm_t * pcm, const void *buffer,
					snd_pcm_uframes_t size);

struct AlsaOutput {
	struct audio_output base;

	Manual<PcmExport> pcm_export;

	/**
	 * The configured name of the ALSA device; empty for the
	 * default device
	 */
	std::string device;

	/** use memory mapped I/O? */
	bool use_mmap;

	/**
	 * Enable DSD over USB according to the dCS suggested
	 * standard?
	 *
	 * @see http://www.dcsltd.co.uk/page/assets/DSDoverUSB.pdf
	 */
	bool dsd_usb;

	/**
	 * Enable native DSD playback support (requires ALSA driver support)
	 *
	 */

	bool dsd_native;

	/** libasound's buffer_time setting (in microseconds) */
	unsigned int buffer_time;

	/** libasound's period_time setting (in microseconds) */
	unsigned int period_time;

	/** the mode flags passed to snd_pcm_open */
	int mode;

	/** the libasound PCM device handle */
	snd_pcm_t *pcm;

	/**
	 * a pointer to the libasound writei() function, which is
	 * snd_pcm_writei() or snd_pcm_mmap_writei(), depending on the
	 * use_mmap configuration
	 */
	alsa_writei_t *writei;

	/**
	 * The size of one audio frame passed to method play().
	 */
	size_t in_frame_size;

	/**
	 * The size of one audio frame passed to libasound.
	 */
	size_t out_frame_size;

	/**
	 * The size of one period, in number of frames.
	 */
	snd_pcm_uframes_t period_frames;

	/**
	 * The number of frames written in the current period.
	 */
	snd_pcm_uframes_t period_position;

	/**
	 * Do we need to call snd_pcm_prepare() before the next write?
	 * It means that we put the device to SND_PCM_STATE_SETUP by
	 * calling snd_pcm_drop().
	 *
	 * Without this flag, we could easily recover after a failed
	 * optimistic write (returning -EBADFD), but the Raspberry Pi
	 * audio driver is infamous for generating ugly artefacts from
	 * this.
	 */
	bool must_prepare;

	/**
	 * This buffer gets allocated after opening the ALSA device.
	 * It contains silence samples, enough to fill one period (see
	 * #period_frames).
	 */
	void *silence;

	AlsaOutput():mode(0), writei(snd_pcm_writei) {
	}

	bool Init(const config_param &param, Error &error) {
		return ao_base_init(&base, &alsa_output_plugin,
				    param, error);
	}

	void Deinit() {
		ao_base_finish(&base);
	}
};

static constexpr Domain alsa_output_domain("alsa_output");

static const char *
alsa_device(const AlsaOutput *ad)
{
	return ad->device.empty() ? default_device : ad->device.c_str();
}

static void
alsa_configure(AlsaOutput *ad, const config_param &param)
{
	ad->device = param.GetBlockValue("device", "");

	ad->use_mmap = param.GetBlockValue("use_mmap", false);

	ad->dsd_usb = param.GetBlockValue("dsd_usb", false);

	ad->dsd_native = param.GetBlockValue("dsd_native", false);

	/* If both dsd_usb and dsd_native are enabled, fall back to dsd_usb */
	if (ad->dsd_usb && ad->dsd_native)
		ad->dsd_native = false;

	ad->buffer_time = param.GetBlockValue("buffer_time",
					      MPD_ALSA_BUFFER_TIME_US);
	ad->period_time = param.GetBlockValue("period_time", 0u);

#ifdef SND_PCM_NO_AUTO_RESAMPLE
	if (!param.GetBlockValue("auto_resample", true))
		ad->mode |= SND_PCM_NO_AUTO_RESAMPLE;
#endif

#ifdef SND_PCM_NO_AUTO_CHANNELS
	if (!param.GetBlockValue("auto_channels", true))
		ad->mode |= SND_PCM_NO_AUTO_CHANNELS;
#endif

#ifdef SND_PCM_NO_AUTO_FORMAT
	if (!param.GetBlockValue("auto_format", true))
		ad->mode |= SND_PCM_NO_AUTO_FORMAT;
#endif
}

static struct audio_output *
alsa_init(const config_param &param, Error &error)
{
	AlsaOutput *ad = new AlsaOutput();

	if (!ad->Init(param, error)) {
		delete ad;
		return nullptr;
	}

	alsa_configure(ad, param);

	return &ad->base;
}

static void
alsa_finish(struct audio_output *ao)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	ad->Deinit();
	delete ad;

	/* free libasound's config cache */
	snd_config_update_free_global();
}

static bool
alsa_output_enable(struct audio_output *ao, gcc_unused Error &error)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	ad->pcm_export.Construct();
	return true;
}

static void
alsa_output_disable(struct audio_output *ao)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	ad->pcm_export.Destruct();
}

static bool
alsa_test_default_device(void)
{
	snd_pcm_t *handle;

	int ret = snd_pcm_open(&handle, default_device,
	                       SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret) {
		FormatError(alsa_output_domain,
			    "Error opening default ALSA device: %s",
			    snd_strerror(-ret));
		return false;
	} else
		snd_pcm_close(handle);

	return true;
}

static snd_pcm_format_t
get_bitformat(SampleFormat sample_format)
{
	switch (sample_format) {
	case SampleFormat::UNDEFINED:
	case SampleFormat::DSD:
		return SND_PCM_FORMAT_UNKNOWN;

	case SampleFormat::S8:
		return SND_PCM_FORMAT_S8;

	case SampleFormat::S16:
		return SND_PCM_FORMAT_S16;

	case SampleFormat::S24_P32:
		return SND_PCM_FORMAT_S24;

	case SampleFormat::S32:
		return SND_PCM_FORMAT_S32;

	case SampleFormat::FLOAT:
		return SND_PCM_FORMAT_FLOAT;

	case SampleFormat::DSD_U8:
		return SND_PCM_FORMAT_DSD_U8;
	}

	assert(false);
	gcc_unreachable();
}

static snd_pcm_format_t
byteswap_bitformat(snd_pcm_format_t fmt)
{
	switch(fmt) {
	case SND_PCM_FORMAT_S16_LE: return SND_PCM_FORMAT_S16_BE;
	case SND_PCM_FORMAT_S24_LE: return SND_PCM_FORMAT_S24_BE;
	case SND_PCM_FORMAT_S32_LE: return SND_PCM_FORMAT_S32_BE;
	case SND_PCM_FORMAT_S16_BE: return SND_PCM_FORMAT_S16_LE;
	case SND_PCM_FORMAT_S24_BE: return SND_PCM_FORMAT_S24_LE;

	case SND_PCM_FORMAT_S24_3BE:
		return SND_PCM_FORMAT_S24_3LE;

	case SND_PCM_FORMAT_S24_3LE:
		return SND_PCM_FORMAT_S24_3BE;

	case SND_PCM_FORMAT_S32_BE: return SND_PCM_FORMAT_S32_LE;
	default: return SND_PCM_FORMAT_UNKNOWN;
	}
}

static snd_pcm_format_t
alsa_to_packed_format(snd_pcm_format_t fmt)
{
	switch (fmt) {
	case SND_PCM_FORMAT_S24_LE:
		return SND_PCM_FORMAT_S24_3LE;

	case SND_PCM_FORMAT_S24_BE:
		return SND_PCM_FORMAT_S24_3BE;

	default:
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static int
alsa_try_format_or_packed(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
			  snd_pcm_format_t fmt, bool *packed_r)
{
	int err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		*packed_r = false;

	if (err != -EINVAL)
		return err;

	fmt = alsa_to_packed_format(fmt);
	if (fmt == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		*packed_r = true;

	return err;
}

/**
 * Attempts to configure the specified sample format, and tries the
 * reversed host byte order if was not supported.
 */
static int
alsa_output_try_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		       SampleFormat sample_format,
		       bool *packed_r, bool *reverse_endian_r)
{
	snd_pcm_format_t alsa_format = get_bitformat(sample_format);
	if (alsa_format == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	int err = alsa_try_format_or_packed(pcm, hwparams, alsa_format,
					    packed_r);
	if (err == 0)
		*reverse_endian_r = false;

	if (err != -EINVAL)
		return err;

	alsa_format = byteswap_bitformat(alsa_format);
	if (alsa_format == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = alsa_try_format_or_packed(pcm, hwparams, alsa_format, packed_r);
	if (err == 0)
		*reverse_endian_r = true;

	return err;
}

/**
 * Configure a sample format, and probe other formats if that fails.
 */
static int
alsa_output_setup_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
			 AudioFormat &audio_format,
			 bool *packed_r, bool *reverse_endian_r)
{
	/* try the input format first */

	int err = alsa_output_try_format(pcm, hwparams,
					 audio_format.format,
					 packed_r, reverse_endian_r);

	/* if unsupported by the hardware, try other formats */

	static const SampleFormat probe_formats[] = {
		SampleFormat::S24_P32,
		SampleFormat::S32,
		SampleFormat::S16,
		SampleFormat::S8,
		SampleFormat::UNDEFINED,
	};

	for (unsigned i = 0;
	     err == -EINVAL && probe_formats[i] != SampleFormat::UNDEFINED;
	     ++i) {
		const SampleFormat mpd_format = probe_formats[i];
		if (mpd_format == audio_format.format)
			continue;

		err = alsa_output_try_format(pcm, hwparams, mpd_format,
					     packed_r, reverse_endian_r);
		if (err == 0)
			audio_format.format = mpd_format;
	}

	return err;
}

/**
 * Set up the snd_pcm_t object which was opened by the caller.  Set up
 * the configured settings and the audio format.
 */
static bool
alsa_setup(AlsaOutput *ad, AudioFormat &audio_format,
	   bool *packed_r, bool *reverse_endian_r, Error &error)
{
	unsigned int sample_rate = audio_format.sample_rate;
	unsigned int channels = audio_format.channels;
	int err;
	const char *cmd = nullptr;
	int retry = MPD_ALSA_RETRY_NR;
	unsigned int period_time, period_time_ro;
	unsigned int buffer_time;

	period_time_ro = period_time = ad->period_time;
configure_hw:
	/* configure HW params */
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_hw_params_alloca(&hwparams);
	cmd = "snd_pcm_hw_params_any";
	err = snd_pcm_hw_params_any(ad->pcm, hwparams);
	if (err < 0)
		goto error;

	if (ad->use_mmap) {
		err = snd_pcm_hw_params_set_access(ad->pcm, hwparams,
						   SND_PCM_ACCESS_MMAP_INTERLEAVED);
		if (err < 0) {
			FormatWarning(alsa_output_domain,
				      "Cannot set mmap'ed mode on ALSA device \"%s\": %s",
				      alsa_device(ad), snd_strerror(-err));
			LogWarning(alsa_output_domain,
				   "Falling back to direct write mode");
			ad->use_mmap = false;
		} else
			ad->writei = snd_pcm_mmap_writei;
	}

	if (!ad->use_mmap) {
		cmd = "snd_pcm_hw_params_set_access";
		err = snd_pcm_hw_params_set_access(ad->pcm, hwparams,
						   SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0)
			goto error;
		ad->writei = snd_pcm_writei;
	}

	err = alsa_output_setup_format(ad->pcm, hwparams, audio_format,
				       packed_r, reverse_endian_r);
	if (err < 0) {
		error.Format(alsa_output_domain, err,
			     "ALSA device \"%s\" does not support format %s: %s",
			     alsa_device(ad),
			     sample_format_to_string(audio_format.format),
			     snd_strerror(-err));
		return false;
	}

	snd_pcm_format_t format;
	if (snd_pcm_hw_params_get_format(hwparams, &format) == 0)
		FormatDebug(alsa_output_domain,
			    "format=%s (%s)", snd_pcm_format_name(format),
			    snd_pcm_format_description(format));

	err = snd_pcm_hw_params_set_channels_near(ad->pcm, hwparams,
						  &channels);
	if (err < 0) {
		error.Format(alsa_output_domain, err,
			     "ALSA device \"%s\" does not support %i channels: %s",
			     alsa_device(ad), (int)audio_format.channels,
			     snd_strerror(-err));
		return false;
	}
	audio_format.channels = (int8_t)channels;

	err = snd_pcm_hw_params_set_rate_near(ad->pcm, hwparams,
					      &sample_rate, nullptr);
	if (err < 0 || sample_rate == 0) {
		error.Format(alsa_output_domain, err,
			     "ALSA device \"%s\" does not support %u Hz audio",
			     alsa_device(ad), audio_format.sample_rate);
		return false;
	}
	audio_format.sample_rate = sample_rate;

	snd_pcm_uframes_t buffer_size_min, buffer_size_max;
	snd_pcm_hw_params_get_buffer_size_min(hwparams, &buffer_size_min);
	snd_pcm_hw_params_get_buffer_size_max(hwparams, &buffer_size_max);
	unsigned buffer_time_min, buffer_time_max;
	snd_pcm_hw_params_get_buffer_time_min(hwparams, &buffer_time_min, 0);
	snd_pcm_hw_params_get_buffer_time_max(hwparams, &buffer_time_max, 0);
	FormatDebug(alsa_output_domain, "buffer: size=%u..%u time=%u..%u",
		    (unsigned)buffer_size_min, (unsigned)buffer_size_max,
		    buffer_time_min, buffer_time_max);

	snd_pcm_uframes_t period_size_min, period_size_max;
	snd_pcm_hw_params_get_period_size_min(hwparams, &period_size_min, 0);
	snd_pcm_hw_params_get_period_size_max(hwparams, &period_size_max, 0);
	unsigned period_time_min, period_time_max;
	snd_pcm_hw_params_get_period_time_min(hwparams, &period_time_min, 0);
	snd_pcm_hw_params_get_period_time_max(hwparams, &period_time_max, 0);
	FormatDebug(alsa_output_domain, "period: size=%u..%u time=%u..%u",
		    (unsigned)period_size_min, (unsigned)period_size_max,
		    period_time_min, period_time_max);

	if (ad->buffer_time > 0) {
		buffer_time = ad->buffer_time;
		cmd = "snd_pcm_hw_params_set_buffer_time_near";
		err = snd_pcm_hw_params_set_buffer_time_near(ad->pcm, hwparams,
							     &buffer_time, nullptr);
		if (err < 0)
			goto error;
	} else {
		err = snd_pcm_hw_params_get_buffer_time(hwparams, &buffer_time,
							nullptr);
		if (err < 0)
			buffer_time = 0;
	}

	if (period_time_ro == 0 && buffer_time >= 10000) {
		period_time_ro = period_time = buffer_time / 4;

		FormatDebug(alsa_output_domain,
			    "default period_time = buffer_time/4 = %u/4 = %u",
			    buffer_time, period_time);
	}

	if (period_time_ro > 0) {
		period_time = period_time_ro;
		cmd = "snd_pcm_hw_params_set_period_time_near";
		err = snd_pcm_hw_params_set_period_time_near(ad->pcm, hwparams,
							     &period_time, nullptr);
		if (err < 0)
			goto error;
	}

	cmd = "snd_pcm_hw_params";
	err = snd_pcm_hw_params(ad->pcm, hwparams);
	if (err == -EPIPE && --retry > 0 && period_time_ro > 0) {
		period_time_ro = period_time_ro >> 1;
		goto configure_hw;
	} else if (err < 0)
		goto error;
	if (retry != MPD_ALSA_RETRY_NR)
		FormatDebug(alsa_output_domain,
			    "ALSA period_time set to %d", period_time);

	snd_pcm_uframes_t alsa_buffer_size;
	cmd = "snd_pcm_hw_params_get_buffer_size";
	err = snd_pcm_hw_params_get_buffer_size(hwparams, &alsa_buffer_size);
	if (err < 0)
		goto error;

	snd_pcm_uframes_t alsa_period_size;
	cmd = "snd_pcm_hw_params_get_period_size";
	err = snd_pcm_hw_params_get_period_size(hwparams, &alsa_period_size,
						nullptr);
	if (err < 0)
		goto error;

	/* configure SW params */
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sw_params_alloca(&swparams);

	cmd = "snd_pcm_sw_params_current";
	err = snd_pcm_sw_params_current(ad->pcm, swparams);
	if (err < 0)
		goto error;

	cmd = "snd_pcm_sw_params_set_start_threshold";
	err = snd_pcm_sw_params_set_start_threshold(ad->pcm, swparams,
						    alsa_buffer_size -
						    alsa_period_size);
	if (err < 0)
		goto error;

	cmd = "snd_pcm_sw_params_set_avail_min";
	err = snd_pcm_sw_params_set_avail_min(ad->pcm, swparams,
					      alsa_period_size);
	if (err < 0)
		goto error;

	cmd = "snd_pcm_sw_params";
	err = snd_pcm_sw_params(ad->pcm, swparams);
	if (err < 0)
		goto error;

	FormatDebug(alsa_output_domain, "buffer_size=%u period_size=%u",
		    (unsigned)alsa_buffer_size, (unsigned)alsa_period_size);

	if (alsa_period_size == 0)
		/* this works around a SIGFPE bug that occurred when
		   an ALSA driver indicated period_size==0; this
		   caused a division by zero in alsa_play().  By using
		   the fallback "1", we make sure that this won't
		   happen again. */
		alsa_period_size = 1;

	ad->period_frames = alsa_period_size;
	ad->period_position = 0;

	ad->silence = g_malloc(snd_pcm_frames_to_bytes(ad->pcm,
						       alsa_period_size));
	snd_pcm_format_set_silence(format, ad->silence,
				   alsa_period_size * channels);

	return true;

error:
	error.Format(alsa_output_domain, err,
		     "Error opening ALSA device \"%s\" (%s): %s",
		     alsa_device(ad), cmd, snd_strerror(-err));
	return false;
}

static bool
alsa_setup_dsd(AlsaOutput *ad, const AudioFormat audio_format,
	       bool *shift8_r, bool *packed_r, bool *reverse_endian_r,
	       Error &error)
{
	assert(ad->dsd_usb);
	assert(audio_format.format == SampleFormat::DSD);

	AudioFormat usb_format = audio_format;

	if (ad->dsd_native) {

		usb_format.format = SampleFormat::DSD_U8;
		if (!alsa_setup(ad, usb_format, packed_r, reverse_endian_r, error)) {

			error.Format(alsa_output_domain,
				"Failed to configure native DSD playback on ALSA device \"%s\"",
				alsa_device(ad));
			g_free(ad->silence);
			return false;
		}
		return true;
	}

	/* pass 24 bit to alsa_setup() */

	usb_format.format = SampleFormat::S24_P32;
	usb_format.sample_rate /= 2;

	const AudioFormat check = usb_format;

	if (!alsa_setup(ad, usb_format, packed_r, reverse_endian_r, error))
		return false;

	/* if the device allows only 32 bit, shift all DSD-over-USB
	   samples left by 8 bit and leave the lower 8 bit cleared;
	   the DSD-over-USB documentation does not specify whether
	   this is legal, but there is anecdotical evidence that this
	   is possible (and the only option for some devices) */
	*shift8_r = usb_format.format == SampleFormat::S32;
	if (usb_format.format == SampleFormat::S32)
		usb_format.format = SampleFormat::S24_P32;

	if (usb_format != check) {
		/* no bit-perfect playback, which is required
		   for DSD over USB */
		error.Format(alsa_output_domain,
			     "Failed to configure DSD-over-USB on ALSA device \"%s\"",
			     alsa_device(ad));
		g_free(ad->silence);
		return false;
	}

	return true;
}

static bool
alsa_setup_or_dsd(AlsaOutput *ad, AudioFormat &audio_format,
		  Error &error)
{
	bool shift8 = false, packed, reverse_endian;

	const bool dsd_usb = ad->dsd_usb &&
		audio_format.format == SampleFormat::DSD;

	const bool dsd_native = ad->dsd_native &&
		audio_format.format == SampleFormat::DSD;

	const bool dsd_enabled = dsd_usb || dsd_native;

	const bool success = dsd_enabled
		? alsa_setup_dsd(ad, audio_format,
				 &shift8, &packed, &reverse_endian,
				 error)
		: alsa_setup(ad, audio_format, &packed, &reverse_endian,
			     error);
	if (!success)
		return false;

	ad->pcm_export->Open(audio_format.format,
			     audio_format.channels,
			     dsd_usb, shift8, packed, reverse_endian);
	return true;
}

static bool
alsa_open(struct audio_output *ao, AudioFormat &audio_format, Error &error)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	int err = snd_pcm_open(&ad->pcm, alsa_device(ad),
			       SND_PCM_STREAM_PLAYBACK, ad->mode);
	if (err < 0) {
		error.Format(alsa_output_domain, err,
			    "Failed to open ALSA device \"%s\": %s",
			    alsa_device(ad), snd_strerror(err));
		return false;
	}

	FormatDebug(alsa_output_domain, "opened %s type=%s",
		    snd_pcm_name(ad->pcm),
		    snd_pcm_type_name(snd_pcm_type(ad->pcm)));

	if (!alsa_setup_or_dsd(ad, audio_format, error)) {
		snd_pcm_close(ad->pcm);
		return false;
	}

	ad->in_frame_size = audio_format.GetFrameSize();
	ad->out_frame_size = ad->pcm_export->GetFrameSize(audio_format);

	ad->must_prepare = false;

	return true;
}

/**
 * Write silence to the ALSA device.
 */
static void
alsa_write_silence(AlsaOutput *ad, snd_pcm_uframes_t nframes)
{
	ad->writei(ad->pcm, ad->silence, nframes);
}

static int
alsa_recover(AlsaOutput *ad, int err)
{
	if (err == -EPIPE) {
		FormatDebug(alsa_output_domain,
			    "Underrun on ALSA device \"%s\"", alsa_device(ad));
	} else if (err == -ESTRPIPE) {
		FormatDebug(alsa_output_domain,
			    "ALSA device \"%s\" was suspended",
			    alsa_device(ad));
	}

	switch (snd_pcm_state(ad->pcm)) {
	case SND_PCM_STATE_PAUSED:
		err = snd_pcm_pause(ad->pcm, /* disable */ 0);
		break;
	case SND_PCM_STATE_SUSPENDED:
		err = snd_pcm_resume(ad->pcm);
		if (err == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		ad->period_position = 0;
		err = snd_pcm_prepare(ad->pcm);
		break;
	case SND_PCM_STATE_DISCONNECTED:
		break;
	/* this is no error, so just keep running */
	case SND_PCM_STATE_RUNNING:
		err = 0;
		break;
	default:
		/* unknown state, do nothing */
		break;
	}

	return err;
}

static void
alsa_drain(struct audio_output *ao)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	if (snd_pcm_state(ad->pcm) != SND_PCM_STATE_RUNNING)
		return;

	if (ad->period_position > 0) {
		/* generate some silence to finish the partial
		   period */
		snd_pcm_uframes_t nframes =
			ad->period_frames - ad->period_position;
		alsa_write_silence(ad, nframes);
	}

	snd_pcm_drain(ad->pcm);

	ad->period_position = 0;
}

static void
alsa_cancel(struct audio_output *ao)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	ad->period_position = 0;
	ad->must_prepare = true;

	snd_pcm_drop(ad->pcm);
}

static void
alsa_close(struct audio_output *ao)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	snd_pcm_close(ad->pcm);
	g_free(ad->silence);
}

static size_t
alsa_play(struct audio_output *ao, const void *chunk, size_t size,
	  Error &error)
{
	AlsaOutput *ad = (AlsaOutput *)ao;

	assert(size > 0);
	assert(size % ad->in_frame_size == 0);

	if (ad->must_prepare) {
		ad->must_prepare = false;

		int err = snd_pcm_prepare(ad->pcm);
		if (err < 0) {
			error.Set(alsa_output_domain, err, snd_strerror(-err));
			return 0;
		}
	}

	const size_t original_size = size;
	chunk = ad->pcm_export->Export(chunk, size, size);
	if (size == 0)
		/* the DoP (DSD over PCM) filter converts two frames
		   at a time and ignores the last odd frame; if there
		   was only one frame (e.g. the last frame in the
		   file), the result is empty; to avoid an endless
		   loop, bail out here, and pretend the one frame has
		   been played */
		return original_size;

	assert(size % ad->out_frame_size == 0);

	size /= ad->out_frame_size;
	assert(size > 0);

	while (true) {
		snd_pcm_sframes_t ret = ad->writei(ad->pcm, chunk, size);
		if (ret > 0) {
			ad->period_position = (ad->period_position + ret)
				% ad->period_frames;

			size_t bytes_written = ret * ad->out_frame_size;
			return ad->pcm_export->CalcSourceSize(bytes_written);
		}

		if (ret < 0 && ret != -EAGAIN && ret != -EINTR &&
		    alsa_recover(ad, ret) < 0) {
			error.Set(alsa_output_domain, ret, snd_strerror(-ret));
			return 0;
		}
	}
}

const struct audio_output_plugin alsa_output_plugin = {
	"alsa",
	alsa_test_default_device,
	alsa_init,
	alsa_finish,
	alsa_output_enable,
	alsa_output_disable,
	alsa_open,
	alsa_close,
	nullptr,
	nullptr,
	alsa_play,
	alsa_drain,
	alsa_cancel,
	nullptr,

	&alsa_mixer_plugin,
};
