/*-
 * Copyright (c) 2016 Jared McNeill <jmcneill@invisible.ca>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define	LIBRARY_NAME		"V4L2"
#define	LIBRARY_VERSION		"0.0.1"
#define	VIDEO_PATH		"/dev/video0"
#define	VIDEO_FPS		(60/1.001)
#define	AUDIO_DEVICE		"hw:1,0"
#define	AUDIO_SAMPLE_RATE	48000
#define	AUDIO_BUFSIZE		64

#include "libretro.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/videodev2.h>
#include <libv4l2.h>

#include <alsa/asoundlib.h>

static int video_fd = -1;
static struct v4l2_format video_format;
static uint8_t *video_data;
static size_t video_data_len;
static uint16_t *conv_data;

static snd_pcm_t *audio_handle;

static retro_environment_t environment_cb;
static retro_video_refresh_t video_refresh_cb;
static retro_audio_sample_t audio_sample_cb;
static retro_audio_sample_batch_t audio_sample_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static void
audio_callback(void)
{
	int16_t audio_data[64];
	int i, frame;

	if (audio_handle) {
		const int frames = snd_pcm_readi(audio_handle, audio_data, sizeof(audio_data) / 4);
		for (frame = 0, i = 0; frame < frames; frame++, i += 2)
			audio_sample_cb(audio_data[i+0], audio_data[i+1]);
	}
}

static void
audio_set_state(bool enable)
{
}

RETRO_API void
retro_set_environment(retro_environment_t cb)
{
	struct retro_audio_callback audio_cb;

	environment_cb = cb;

	audio_cb.callback = audio_callback;
	audio_cb.set_state = audio_set_state;
	environment_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);
}

RETRO_API void
retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_refresh_cb = cb;
}

RETRO_API void
retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_sample_cb = cb;
}

RETRO_API void
retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_sample_batch_cb = cb;
}

RETRO_API void
retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

RETRO_API void
retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

RETRO_API void
retro_init(void)
{
	struct v4l2_capability caps;
	snd_pcm_hw_params_t *hw_params;
	unsigned int rate;
	int error;

	video_fd = v4l2_open(VIDEO_PATH, 0);
	if (video_fd == -1) {
		printf("Couldn't open " VIDEO_PATH ": %s\n", strerror(errno));
		abort();
	}

	error = v4l2_ioctl(video_fd, VIDIOC_QUERYCAP, &caps);
	if (error != 0) {
		printf("VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
		abort();
	}

	printf("%s:\n", VIDEO_PATH);
	printf("  Driver: %s\n", caps.driver);
	printf("  Card: %s\n", caps.card);
	printf("  Bus Info: %s\n", caps.bus_info);
	printf("  Version: %u.%u.%u\n", (caps.version >> 16) & 0xff, (caps.version >> 8) & 0xff, caps.version & 0xff);

	error = snd_pcm_open(&audio_handle, AUDIO_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
	if (error < 0) {
		printf("Couldn't open " AUDIO_DEVICE ": %s\n", snd_strerror(error));
		audio_handle = NULL;
	}

	if (audio_handle) {
		error = snd_pcm_hw_params_malloc(&hw_params);
		if (error) {
			printf("Couldn't allocate hw param structure: %s\n", snd_strerror(error));
			abort();
		}
		error = snd_pcm_hw_params_any(audio_handle, hw_params);
		if (error) {
			printf("Couldn't initialize hw param structure: %s\n", snd_strerror(error));
			abort();
		}
		error = snd_pcm_hw_params_set_access(audio_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
		if (error) {
			printf("Couldn't set hw param access type: %s\n", snd_strerror(error));
			abort();
		}
		error = snd_pcm_hw_params_set_format(audio_handle, hw_params, SND_PCM_FORMAT_S16_LE);
		if (error) {	
			printf("Couldn't set hw param format to SND_PCM_FORMAT_S16_LE: %s\n", snd_strerror(error));
			abort();
		}
		rate = AUDIO_SAMPLE_RATE;
		error = snd_pcm_hw_params_set_rate_near(audio_handle, hw_params, &rate, 0);
		if (error) {
			printf("Couldn't set hw param sample rate to %u: %s\n", rate, snd_strerror(error));
			abort();
		}
		if (rate != AUDIO_SAMPLE_RATE) {
			printf("Hardware doesn't support sample rate %u (returned %u)\n", AUDIO_SAMPLE_RATE, rate);
			abort();
		}
		error = snd_pcm_hw_params_set_channels(audio_handle, hw_params, 2);
		if (error) {
			printf("Couldn't set hw param channels to 2: %s\n", snd_strerror(error));
			abort();
		}
		error = snd_pcm_hw_params(audio_handle, hw_params);
		if (error) {
			printf("Couldn't set hw params: %s\n", snd_strerror(error));
			abort();
		}
		snd_pcm_hw_params_free(hw_params);

		error = snd_pcm_prepare(audio_handle);
		if (error) {
			printf("Couldn't prepare audio interface for use: %s\n", snd_strerror(error));
			abort();
		}

		printf("Using ALSA for audio input\n");
	}
}

RETRO_API void
retro_deinit(void)
{
	if (audio_handle)
		snd_pcm_close(audio_handle);

	if (video_fd != -1)
		v4l2_close(video_fd);
}

RETRO_API unsigned
retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void
retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = LIBRARY_NAME;
	info->library_version = LIBRARY_VERSION;
	info->valid_extensions = "";
	info->need_fullpath = true;
	info->block_extract = true;
}

RETRO_API void
retro_get_system_av_info(struct retro_system_av_info *info)
{
	struct v4l2_cropcap cc;
	int error;

	memset(&cc, 0, sizeof(cc));
	cc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	error = v4l2_ioctl(video_fd, VIDIOC_CROPCAP, &cc);
	if (error == 0) {
		info->geometry.aspect_ratio = (double)cc.pixelaspect.denominator / (double)cc.pixelaspect.numerator;
	}

	info->geometry.base_width = info->geometry.max_width = video_format.fmt.pix.width;
	info->geometry.base_height = info->geometry.max_height = video_format.fmt.pix.height;
	info->timing.fps = VIDEO_FPS;
	info->timing.sample_rate = AUDIO_SAMPLE_RATE;

	printf("Resolution %ux%u %f fps\n", info->geometry.base_width, info->geometry.base_height, info->timing.fps);
}

RETRO_API void
retro_set_controller_port_device(unsigned port, unsigned device)
{
}

RETRO_API void
retro_reset(void)
{
}

RETRO_API void
retro_run(void)
{
	ssize_t len;
	uint8_t *src;
	uint16_t *dst;
	int i;

	input_poll_cb();

	if (video_data == NULL || video_fd == -1)
		return;

	len = v4l2_read(video_fd, video_data, video_format.fmt.pix.sizeimage);
	if (len == -1)
		printf("v4l2_read error: %s\n", strerror(errno));

	src = video_data;
	dst = conv_data;

#ifdef PLUGIN_RGB24
	/* RGB24 to RGB565 */
	for (i = 0; i < video_format.fmt.pix.width * video_format.fmt.pix.height; i++, src += 3, dst += 1) {
		*dst = ((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | ((src[2] >> 3) << 0);
	}
#else
	/* UYVY to RGB565 */
	const int K1 = (int)(1.402f * (1 << 16));
	const int K2 = (int)(0.714f * (1 << 16));
	const int K3 = (int)(0.334f * (1 << 16));
	const int K4 = (int)(1.772f * (1 << 16));
	int R, G, B;
	for (i = 0; i < video_format.fmt.pix.width * video_format.fmt.pix.height; i += 2, src += 4) {
		const uint8_t U = src[0];
		const uint8_t Y1 = src[1];
		const uint8_t V = src[2];
		const uint8_t Y2 = src[3];

		const int8_t uf = U - 128;
		const int8_t vf = V - 128;

		R = Y1 + (K1 * vf >> 16);
		G = Y1 - (K2 * vf >> 16) - (K3 * uf >> 16);
		B = Y1 + (K4 * uf >> 16);

		if (R < 0) R = 0;
		else if (R > 255) R = 255;
		if (G < 0) G = 0;
		else if (G > 255) G = 255;
		if (B < 0) B = 0;
		else if (B > 255) B = 255;

		*dst = ((R >> 3) << 11) | ((G >> 2) << 5) | ((B >> 3) << 0);
		dst++;

		R = Y2 + (K1 * vf >> 16);
		G = Y2 - (K2 * vf >> 16) - (K3 * uf >> 16);
		B = Y2 + (K4 * uf >> 16);
	
		if (R < 0) R = 0;
		else if (R > 255) R = 255;
		if (G < 0) G = 0;
		else if (G > 255) G = 255;
		if (B < 0) B = 0;
		else if (B > 255) B = 255;

		*dst = ((R >> 3) << 11) | ((G >> 2) << 5) | ((B >> 3) << 0);
		dst++;
	}
#endif

	video_refresh_cb(conv_data, video_format.fmt.pix.width, video_format.fmt.pix.height, video_format.fmt.pix.width * 2);
}

RETRO_API size_t
retro_serialize_size(void)
{
	return 0;
}

RETRO_API bool
retro_serialize(void *data, size_t size)
{
	return false;
}

RETRO_API bool
retro_unserialize(const void *data, size_t size)
{
	return false;
}

RETRO_API void
retro_cheat_reset(void)
{
}

RETRO_API void
retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

RETRO_API bool
retro_load_game(const struct retro_game_info *game)
{
	enum retro_pixel_format pixel_format;
	struct v4l2_standard std, *pstd = NULL;
	struct v4l2_format fmt;
	v4l2_std_id std_id;
	uint32_t index;
	int error;

	if (video_fd == -1) {
		printf("Video device not opened\n");
		return false;
	}

	free(video_data);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	error = v4l2_ioctl(video_fd, VIDIOC_G_FMT, &fmt);
	if (error != 0) {
		printf("VIDIOC_G_FMT failed: %s\n", strerror(errno));
		return false;
	}
#ifdef PLUGIN_RGB24
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	error = v4l2_ioctl(video_fd, VIDIOC_S_FMT, &fmt);
	if (error != 0) {
		printf("VIDIOC_S_FMT failed: %s\n", strerror(errno));
		return false;
	}
#endif

	error = v4l2_ioctl(video_fd, VIDIOC_G_STD, &std_id);
	if (error != 0) {
		printf("VIDIOC_G_STD failed: %s\n", strerror(errno));
		return false;
	}
	for (index = 0; ; index++) {
		memset(&std, 0, sizeof(std));
		std.index = index;
		error = v4l2_ioctl(video_fd, VIDIOC_ENUMSTD, &std);
		if (error)
			break;
		if (std.id == std_id)
			pstd = &std;
		printf("VIDIOC_ENUMSTD[%u]: %s%s\n", index, std.name, std.id == std_id ? " [*]" : "");
	}
	if (!pstd) {
		printf("VIDIOC_ENUMSTD did not contain std ID %08x\n", (unsigned)std_id);
		return false;
	}

	video_format = fmt;
	video_data_len = video_format.fmt.pix.sizeimage;
	video_data = calloc(1, video_data_len);
	if (video_data == NULL) {
		printf("Cannot allocate video buffer\n");
		return false;
	}
	printf("Allocated %u byte video buffer\n", video_data_len);

	conv_data = calloc(1, video_format.fmt.pix.width * video_format.fmt.pix.height * 2);	
	if (conv_data == NULL) {
		printf("Cannot allocate conversion buffer\n");
		return false;
	}
	printf("Allocated %u byte conversion buffer\n", video_data_len);

	pixel_format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format)) {
		printf("Cannot set pixel format\n");
		return false;
	}

	return true;
}

RETRO_API void
retro_unload_game(void)
{
	free(video_data);
	video_data = NULL;
	free(conv_data);
	conv_data = NULL;
}

RETRO_API bool
retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

RETRO_API unsigned
retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

RETRO_API void *
retro_get_memory_data(unsigned id)
{
	return NULL;
}

RETRO_API size_t
retro_get_memory_size(unsigned id)
{
	return 0;
}
