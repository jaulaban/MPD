/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../output_api.h"
#include "../timer.h"
#include "../utils.h"

struct null_data {
	Timer *timer;
};

static void *null_initDriver(mpd_unused struct audio_output *audioOutput,
			     mpd_unused const struct audio_format *audio_format,
			     mpd_unused ConfigParam *param)
{
	struct null_data *nd = xmalloc(sizeof(*nd));
	nd->timer = NULL;
	return nd;
}

static int null_openDevice(void *data,
			   struct audio_format *audio_format)
{
	struct null_data *nd = data;

	nd->timer = timer_new(audio_format);
	return 0;
}

static void null_closeDevice(void *data)
{
	struct null_data *nd = data;

	if (nd->timer != NULL) {
		timer_free(nd->timer);
		nd->timer = NULL;
	}
}

static int null_playAudio(void *data,
			  mpd_unused const char *playChunk, size_t size)
{
	struct null_data *nd = data;
	Timer *timer = nd->timer;

	if (!timer->started)
		timer_start(timer);
	else
		timer_sync(timer);

	timer_add(timer, size);

	return 0;
}

static void null_dropBufferedAudio(void *data)
{
	struct null_data *nd = data;

	timer_reset(nd->timer);
}

const struct audio_output_plugin nullPlugin = {
	"null",
	NULL,
	null_initDriver,
	NULL,
	null_openDevice,
	null_playAudio,
	null_dropBufferedAudio,
	null_closeDevice,
	NULL,
};
