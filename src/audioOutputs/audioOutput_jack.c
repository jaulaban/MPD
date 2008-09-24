/* jack plug in for the Music Player Daemon (MPD)
 * (c)2006 by anarch(anarchsss@gmail.com)
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

#ifdef HAVE_JACK

#include "../utils.h"
#include "../log.h"

#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>

static const size_t sample_size = sizeof(jack_default_audio_sample_t);

typedef struct _JackData {
	struct audio_output *ao;

	/* configuration */
	const char *name;
	const char *output_ports[2];
	int ringbuf_sz;

	/* for srate() only */
	struct audio_format *audio_format;

	/* locks */
	pthread_mutex_t play_audio_lock;
	pthread_cond_t play_audio;

	/* jack library stuff */
	jack_port_t *ports[2];
	jack_client_t *client;
	jack_ringbuffer_t *ringbuffer[2];
	int bps;
	int shutdown;
} JackData;

/*JackData *jd = NULL;*/

static JackData *newJackData(void)
{
	JackData *ret;

	ret = xcalloc(sizeof(JackData), 1);

	ret->name = "mpd";
	ret->ringbuf_sz = 32768;

	pthread_mutex_init(&ret->play_audio_lock, NULL);
	pthread_cond_init(&ret->play_audio, NULL);

	return ret;
}

static void freeJackClient(JackData *jd)
{
	assert(jd != NULL);

	if (jd->client != NULL) {
		jack_deactivate(jd->client);
		jack_client_close(jd->client);
		jd->client = NULL;
	}

	if (jd->ringbuffer[0] != NULL) {
		jack_ringbuffer_free(jd->ringbuffer[0]);
		jd->ringbuffer[0] = NULL;
	}

	if (jd->ringbuffer[1] != NULL) {
		jack_ringbuffer_free(jd->ringbuffer[1]);
		jd->ringbuffer[1] = NULL;
	}

	pthread_mutex_destroy(&jd->play_audio_lock);
	pthread_cond_destroy(&jd->play_audio);
}

static void freeJackData(JackData *jd)
{
	int i;

	assert(jd != NULL);

	freeJackClient(jd);

	if (strcmp(jd->name, "mpd") != 0)
		xfree(jd->name);

	for ( i = ARRAY_SIZE(jd->output_ports); --i >= 0; ) {
		if (!jd->output_ports[i])
			continue;
		xfree(jd->output_ports[i]);
	}

	free(jd);
}

static void jack_finishDriver(void *data)
{
	JackData *jd = data;
	freeJackData(jd);
	DEBUG("disconnect_jack (pid=%d)\n", getpid ());
}

static int srate(mpd_unused jack_nframes_t rate, void *data)
{
	JackData *jd = (JackData *)data;
	struct audio_format *audioFormat = jd->audio_format;

 	audioFormat->sampleRate = (int)jack_get_sample_rate(jd->client);

	return 0;
}

static int process(jack_nframes_t nframes, void *arg)
{
	size_t i;
	JackData *jd = (JackData *) arg;
	jack_default_audio_sample_t *out[2];
	size_t avail_data, avail_frames;

	if ( nframes <= 0 )
		return 0;

	out[0] = jack_port_get_buffer(jd->ports[0], nframes);
	out[1] = jack_port_get_buffer(jd->ports[1], nframes);

	while ( nframes ) {
		avail_data = jack_ringbuffer_read_space(jd->ringbuffer[1]);

		if ( avail_data > 0 ) {
		    avail_frames = avail_data / sample_size;

		    if (avail_frames > nframes) {
			avail_frames = nframes;
			avail_data = nframes*sample_size;
		    }

		    jack_ringbuffer_read(jd->ringbuffer[0], (char *)out[0],
					 avail_data);
		    jack_ringbuffer_read(jd->ringbuffer[1], (char *)out[1],
					 avail_data);

		    nframes -= avail_frames;
		    out[0] += avail_data;
		    out[1] += avail_data;
		} else {
		    for (i = 0; i < nframes; i++)
  			out[0][i] = out[1][i] = 0.0;
		    nframes = 0;
		}

		if (pthread_mutex_trylock (&jd->play_audio_lock) == 0) {
			pthread_cond_signal (&jd->play_audio);
			pthread_mutex_unlock (&jd->play_audio_lock);
		}
	}


	/*DEBUG("process (pid=%d)\n", getpid());*/
	return 0;
}

static void shutdown_callback(void *arg)
{
	JackData *jd = (JackData *) arg;
	jd->shutdown = 1;
}

static void set_audioformat(JackData *jd, struct audio_format *audioFormat)
{
	audioFormat->sampleRate = (int) jack_get_sample_rate(jd->client);
	DEBUG("samplerate = %d\n", audioFormat->sampleRate);
	audioFormat->channels = 2;
	audioFormat->bits = 16;
	jd->bps = audioFormat->channels
		* sizeof(jack_default_audio_sample_t)
		* audioFormat->sampleRate;
}

static void error_callback(const char *msg)
{
	ERROR("jack: %s\n", msg);
}

static void *jack_initDriver(struct audio_output *ao,
			     mpd_unused const struct audio_format *audio_format,
			     ConfigParam *param)
{
	JackData *jd;
	BlockParam *bp;
	char *endptr;
	int val;
	char *cp = NULL;

	jd = newJackData();
	jd->ao = ao;

	DEBUG("jack_initDriver (pid=%d)\n", getpid());
	if (param == NULL)
		return jd;

	if ( (bp = getBlockParam(param, "ports")) ) {
		DEBUG("output_ports=%s\n", bp->value);

		if (!(cp = strchr(bp->value, ',')))
			FATAL("expected comma and a second value for '%s' "
			      "at line %d: %s\n",
			      bp->name, bp->line, bp->value);

		*cp = '\0';
		jd->output_ports[0] = xstrdup(bp->value);
		*cp++ = ',';

		if (!*cp)
			FATAL("expected a second value for '%s' at line %d: "
			      "%s\n", bp->name, bp->line, bp->value);

		jd->output_ports[1] = xstrdup(cp);

		if (strchr(cp,','))
			FATAL("Only %d values are supported for '%s' "
			      "at line %d\n",
			      (int)ARRAY_SIZE(jd->output_ports),
			      bp->name, bp->line);
	}

	if ( (bp = getBlockParam(param, "ringbuffer_size")) ) {
		errno = 0;
		val = strtol(bp->value, &endptr, 10);

		if ( errno == 0 && endptr != bp->value) {
			jd->ringbuf_sz = val < 32768 ? 32768 : val;
			DEBUG("ringbuffer_size=%d\n", jd->ringbuf_sz);
		} else {
			FATAL("%s is not a number; ringbuf_size=%d\n",
			      bp->value, jd->ringbuf_sz);
		}
	}

	if ( (bp = getBlockParam(param, "name"))
	     && (strcmp(bp->value, "mpd") != 0) ) {
		jd->name = xstrdup(bp->value);
		DEBUG("name=%s\n", jd->name);
	}

	return jd;
}

static int jack_testDefault(void)
{
	return 0;
}

static int connect_jack(JackData *jd, struct audio_format *audio_format)
{
	const char **jports;
	char *port_name;

	jd->audio_format = audio_format;

	if ( (jd->client = jack_client_new(jd->name)) == NULL ) {
		ERROR("jack server not running?\n");
		return -1;
	}

	jack_set_error_function(error_callback);
	jack_set_process_callback(jd->client, process, (void *)jd);
	jack_set_sample_rate_callback(jd->client, (JackProcessCallback)srate,
				      (void *)jd);
	jack_on_shutdown(jd->client, shutdown_callback, (void *)jd);

	if ( jack_activate(jd->client) ) {
		ERROR("cannot activate client\n");
		return -1;
	}

	jd->ports[0] = jack_port_register(jd->client, "left",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);
	if ( !jd->ports[0] ) {
		ERROR("Cannot register left output port.\n");
		return -1;
	}

	jd->ports[1] = jack_port_register(jd->client, "right",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);
	if ( !jd->ports[1] ) {
		ERROR("Cannot register right output port.\n");
		return -1;
	}

	/*  hay que buscar que hay  */
	if ( !jd->output_ports[1]
	     && (jports = jack_get_ports(jd->client, NULL, NULL,
					 JackPortIsPhysical|
					 JackPortIsInput)) ) {
		jd->output_ports[0] = jports[0];
		jd->output_ports[1] = jports[1] ? jports[1] : jports[0];
		DEBUG("output_ports: %s %s\n",
		      jd->output_ports[0], jd->output_ports[1]);
		free(jports);
	}

	if ( jd->output_ports[1] ) {
		jd->ringbuffer[0] = jack_ringbuffer_create(jd->ringbuf_sz);
		jd->ringbuffer[1] = jack_ringbuffer_create(jd->ringbuf_sz);
		memset(jd->ringbuffer[0]->buf, 0, jd->ringbuffer[0]->size);
		memset(jd->ringbuffer[1]->buf, 0, jd->ringbuffer[1]->size);

		port_name = xmalloc(sizeof(char)*(7+strlen(jd->name)));

		sprintf(port_name, "%s:left", jd->name);
		if ( (jack_connect(jd->client, port_name,
				   jd->output_ports[0])) != 0 ) {
			ERROR("%s is not a valid Jack Client / Port\n",
			      jd->output_ports[0]);
			free(port_name);
			return -1;
		}
		sprintf(port_name, "%s:right", jd->name);
		if ( (jack_connect(jd->client, port_name,
				   jd->output_ports[1])) != 0 ) {
			ERROR("%s is not a valid Jack Client / Port\n",
			      jd->output_ports[1]);
			free(port_name);
			return -1;
		}
		free(port_name);
	}

	DEBUG("connect_jack (pid=%d)\n", getpid());
	return 1;
}

static int jack_openDevice(void *data,
			   struct audio_format *audio_format)
{
	JackData *jd = data;

	assert(jd != NULL);

	if (jd->client == NULL && connect_jack(jd, audio_format) < 0) {
		freeJackClient(jd);
		return -1;
	}

	set_audioformat(jd, audio_format);

	DEBUG("jack_openDevice (pid=%d)!\n", getpid ());
	return 0;
}


static void jack_closeDevice(mpd_unused void *data)
{
	/*jack_finishDriver(audioOutput);*/
	DEBUG("jack_closeDevice (pid=%d)\n", getpid());
}

static void jack_dropBufferedAudio (mpd_unused void *data)
{
}

static int jack_playAudio(void *data,
			  const char *buff, size_t size)
{
	JackData *jd = data;
	size_t space;
	size_t i;
	const short *buffer = (const short *) buff;
	jack_default_audio_sample_t sample;
	size_t samples = size/4;

	/*DEBUG("jack_playAudio: (pid=%d)!\n", getpid());*/

	if ( jd->shutdown ) {
		ERROR("Refusing to play, because there is no client thread.\n");
		freeJackClient(jd);
		audio_output_closed(jd->ao);
		return 0;
	}

	while ( samples && !jd->shutdown ) {

 		if ( (space = jack_ringbuffer_write_space(jd->ringbuffer[0]))
 		     >= samples*sample_size ) {

			/*space = MIN(space, samples*sample_size);*/
			/*space = samples*sample_size;*/

			/*for(i=0; i<space/sample_size; i++) {*/
			for(i=0; i<samples; i++) {
				sample = (jack_default_audio_sample_t) *(buffer++)/32768.0;

				jack_ringbuffer_write(jd->ringbuffer[0], (void*)&sample,
						      sample_size);

				sample = (jack_default_audio_sample_t) *(buffer++)/32768.0;

				jack_ringbuffer_write(jd->ringbuffer[1], (void*)&sample,
						      sample_size);

				/*samples--;*/
			}
			samples=0;

 		} else {
			pthread_mutex_lock(&jd->play_audio_lock);
			pthread_cond_wait(&jd->play_audio,
					  &jd->play_audio_lock);
			pthread_mutex_unlock(&jd->play_audio_lock);
		}

	}
	return 0;
}

const struct audio_output_plugin jackPlugin = {
	"jack",
	jack_testDefault,
	jack_initDriver,
	jack_finishDriver,
	jack_openDevice,
	jack_playAudio,
	jack_dropBufferedAudio,
	jack_closeDevice,
	NULL,	/* sendMetadataFunc */
};

#else /* HAVE JACK */

DISABLED_AUDIO_OUTPUT_PLUGIN(jackPlugin)

#endif /* HAVE_JACK */
