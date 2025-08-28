/*
 * audio.c
 *
 *  Created on: 20 Aug 2025
 *      Author: chris
 */

#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "utils.h"
#include "audio.h"

struct audio gAudio;

static int jack_process_frame (jack_nframes_t nframes, void *arg);
static void jack_connect_cb(jack_port_id_t a, jack_port_id_t b, int connect, void *arg);
static void jack_shutdown (void *arg);

/* Compute 2nd order Butterworth low-pass biquad coefficients
 * fc = cutoff frequency (Hz)
 * fs = sample rate (Hz)
*/
void init_lowpass_biquad(double fc, double fs, struct biquad * b)
{
    double omega = 2.0 * M_PI * fc / fs; /* Pre-warp cutoff frequency to normalized rad/s */
    double cos_omega = cos(omega);
    double alpha = sin(omega) / (2.0 * M_SQRT1_2); /* Butterworth (Q = 1/sqrt(2)) */
    double s = 1.0 / (1.0 + alpha); /* normalise for a0 = 1 */
    b->b1 = (ftype)(s * (1.0 - cos_omega));
    b->b0 = b->b2 = (ftype)(b->b1 * 0.5);
    b->a1 = (ftype)(s * -2.0 * cos_omega);
    b->a2 = (ftype)(s * (1.0 - alpha));
    b->z1 = b->z2 = b->y = 0.0;
}

/* can be reinitialised */
void rms_init(struct rms * rms, double samplerate){
	rms->en = true;
	double fc = 6.0; /* -3dB at 6Hz emulates mechanical meter smoothing */
	init_lowpass_biquad(fc, samplerate, &rms->f);
}

void peak_init(struct peak * peak, ftype atten, unsigned decay_samples, unsigned hold_ms){
	peak->peak = peak->_peak = 0.0;
	peak->_hold.tv_sec = 0;
	peak->_hold.tv_nsec = 0;
	peak->hold_time = hold_ms;
	peak->decay_samples = decay_samples;
	peak->_decay = fpow(atten, 1.0/(ftype)decay_samples);
	peak->event = false;
}

void clip_init(struct clip * clip, unsigned threshold) {
	clip->event = false;
	clip->n = 0;
	clip->threshold = threshold;
}

/* run this in background loop, in critical section */
int audio_chan_poll(struct chan * s){
	int events = s->pending;
	s->pending = 0;
	s->rms_val = rms_get(&s->rms);
	peak_get(&s->peak, &s->peak_val); /* ignore updates since these will be accumulated in the event count */
	s->clip_event |= clip_get(&s->clip);
	return events;
}

/* poll at given rate, if theres something to do return > 1, 0 for no events (eg clip/peak), or -1 and errno set */
int audio_poll(struct audio * audio, unsigned period_ms){
	int events = 0;

	/* wait for CV or timeout */
	struct timespec t;
	set_timer(&t, period_ms);
	pthread_mutex_lock(&audio->mutex);
	int err = 0;
	while(!audio->event && !err)
		err = pthread_cond_timedwait(&audio->cond, &audio->mutex, &t);
	if(err && err != ETIMEDOUT){
		errno = err;
		events = -1;
		goto done;
	}
	audio->event = 0;

	if(!audio->disconnected)
		for (int i = 0; i < audio->channels; i++)
			events += audio_chan_poll(&audio->chan[i]);
	else
		events++;
done:
	pthread_mutex_unlock(&audio->mutex);
	return events;
}

int audio_init(struct audio * audio) {
	/* init threading */
	pthread_mutex_init(&audio->mutex, NULL);
	pthread_condattr_t condAttr;
	pthread_condattr_init(&condAttr);
	pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
	pthread_cond_init(&audio->cond, &condAttr);

	/* open a client connection to the JACK server */
	jack_status_t status;
	if(!((audio->jclient = jack_client_open(audio->name, JackNoStartServer, &status, audio->server)))) {
		debug("jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed)
			fprintf (stderr, "Unable to connect to JACK server\n");
		return 1;
	}

	if(status & JackNameNotUnique)
		audio->name = jack_get_client_name(audio->jclient);

	audio->samplerate = jack_get_sample_rate(audio->jclient);

    /* get the list of source ports that match and are active */
	jack_wait_for_source_ports(audio);

	/* count channels */
    for (int i = 0; audio->source_ports[i]; i++)
    	audio->channels++;

    if(audio->level_sinks &&
    		((audio->_level_sink_ports = jack_get_ports(audio->jclient, audio->level_sinks, NULL, JackPortIsInput)))) {
    	for(int i = 0; audio->_level_sink_ports[i]; i++)
    		debug("Route source %d -> sink %s when threshold is reached\n", i+1, audio->_level_sink_ports[i]);
    }

    /* allocate structs */
	if(!((audio->chan = calloc(audio->channels, sizeof(struct chan)))))
		return 1;

	/* register ports per channel */
	for (int i = 0; i < audio->channels; i++) {
		struct chan * c = &audio->chan[i];
		if(audio->vu_peak_hold_ms)
			peak_init(&c->peak, fpow(10.0, -65.0/20.0),
					(int)audio->samplerate*audio->vu_peak_hold_ms/1000,
					audio->vu_peak_hold_ms); /* decay next peak to -65dB in vu_peak_hold_ms */
		if(audio->clip_en)
			clip_init(&c->clip, audio->clip_samples);
		if(audio->rms_en)
			rms_init(&c->rms, (double)audio->samplerate);

		char *in="";
		if(!asprintf(&in, "%d", i+1) ||
				!((c->jport = jack_port_register(audio->jclient, in, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)))){
			debug("Failed to register port %s", in);
			return 1;
		}
	}

	/* start jack callbacks */
	jack_set_process_callback (audio->jclient, jack_process_frame, (void *)audio);
	jack_set_port_connect_callback(audio->jclient, jack_connect_cb, (void *)audio);
	jack_on_shutdown (audio->jclient, jack_shutdown, 0);
	if (jack_activate (audio->jclient)) {
		perror("cannot activate client");
		return 1;
	}
	audio->jack_activated = true;

	/* make the connections */
	jack_connect_source_ports(audio);
    return 0;
}

static int jack_process_frame (jack_nframes_t nframes, void *arg) {
	struct audio * audio = (struct audio *)arg;
	if(!audio->started){
		audio->started = true; /* skip first frame */
		return 0;
	}
	unsigned events=0;

	pthread_mutex_lock(&audio->mutex);
	for (int i=0; i < audio->channels; i++){
		struct chan * c = &audio->chan[i];
		jack_default_audio_sample_t *jbuf = jack_port_get_buffer(c->jport, nframes);
		if(!jbuf)
			goto done;
		for(int s = 0; s < nframes; s++)
			events += audio_chan_run(c, jbuf[s]);
	}
	if(events){
		audio->event += events;
		pthread_cond_broadcast(&audio->cond);
	}
done:
	pthread_mutex_unlock(&audio->mutex);

	return 0;
}

static void jack_connect_cb(jack_port_id_t a, jack_port_id_t b, int connect, void *arg){
	struct audio * audio = (struct audio *)arg;
	if(connect) /* disconnects only */
		return;

	/* check if we disconnected any of audio->source_ports */
    jack_port_t *src = jack_port_by_id(audio->jclient, a);
    jack_port_t *snk = jack_port_by_id(audio->jclient, b);
    if(!src || !snk)
    	return;
    const char *source = jack_port_name(src);
    const char *sink = jack_port_name(snk);
    if(!source || !sink)
    	return;

    for (int i=0; i < audio->channels; i++){
    	if(!strcmp(source, audio->source_ports[i]) && !strcmp(sink, jack_port_name(audio->chan[i].jport))) {
    		fprintf(stderr, "\"%s\" -> \"%s\" Disconnected\n", source, sink);
    		if(audio->noreconnect)
    			exit (EXIT_FAILURE);
    		pthread_mutex_lock(&audio->mutex);
    		audio->disconnected = true;
    		pthread_cond_broadcast(&audio->cond);
    		pthread_mutex_unlock(&audio->mutex);
    		return;
    	}
    }
}

static void jack_shutdown (void *arg) {
	exit (EXIT_FAILURE);
}

/* hold file open until we get an error- in which case we close and try again the next time */
void vu_print(struct audio * audio, const char* fmt, ...){
	if(!gAudio.vu_ms)
		return;

	if(!gAudio._vu && gAudio.vu_pipe){
		if(!((gAudio._vu = fopen(gAudio.vu_pipe, "w+"))))
			return;
	} else
		gAudio._vu = stdout;

	va_list args;

	va_start(args, fmt);
	int r = vfprintf(gAudio._vu, fmt, args);
	va_end (args);
	if(r >= 0)
		return;

	/* error */
	if(gAudio.vu_pipe){
		fclose(gAudio._vu);
		gAudio._vu = NULL;
	}
}

/* 40 cols*/
void vu_print_header(struct audio * audio){
	/* top left, clear page, disable cursor */
	vu_print(audio, "\33[2J\33[H\e[?25l|-80dB    |-60      |-40      |-20    0|\n\n");
}

void vu_console_restore(struct audio * audio){
	/* top left, clear page, disable cursor */
	vu_print(audio, "\33[2J\33[H\e[?25h");
}

void vu_print_pretty(struct audio * audio, ftype rms, ftype peak, int chan){
	int rms_pos = rms < -78.0 ? 0 : (int)(80.0+rms)/2; /* 0 to 40: where 0 is empty, 40 is char 39 */
	int peak_pos = rms < -78.0 ? 0 : (int)(80.0+peak)/2; /* 0 to 40: where 0 is empty, 40 is char 39 */
	char line[41];

	int i = 0;
	if(rms_pos > peak_pos)
		peak_pos = rms_pos;
	while(i < peak_pos){
		line[i] = i == (peak_pos-1) ? '|' : i > rms_pos ?' ' : '*';
		i++;
	}
	if(i==40)
		line[39]='X';
	else if(!i)
		line[i++]='-';
	line[i]=0;
	if(!chan) /* first channel */
		vu_print(audio, "\33[H\n\x1b[2K%s", line); /* top left, one line down, clear line */
	else
		vu_print(audio, "\n\r\x1b[2K%s", line); /* one line down, clear line */
}

/* wait for specified port list to be available */
void jack_wait_for_source_ports(struct audio * audio) {
	if(!audio->sources) /* nothing to wait for */
		return;
reconnect:;
	bool waiting = false;
	if(audio->source_ports)
		jack_free(audio->source_ports);
	while(!((audio->source_ports=jack_get_ports(audio->jclient, audio->sources, NULL, JackPortIsOutput)))) {
		if(!waiting){
			fprintf(stderr, "Wait for ports matching \"%s\"...\n", audio->sources);
			waiting = true;
		}
		millisleep(500);
	}
	if(waiting) /* allow another 500ms to allow all the ports to appear after we see at least one matching */
		goto reconnect;
}

void jack_connect_source_ports(struct audio * audio){
	/* make the connections */
	for (int i = 0; i < audio->channels; i++){
		struct chan * c = &audio->chan[i];
		jack_connect(audio->jclient, audio->source_ports[i], jack_port_name(c->jport));
		debug("connect %s to %s\n", audio->source_ports[i], jack_port_name(c->jport));
	}
}

/* if source ports become disabled, deactivate the jack client, and wait till the ports reappear.
 * Note this does not change the channel count if more matching source channels appear.
 * The channel count is set when this application is started */
void jack_check_source_ports(struct audio * audio){
	if(!audio->disconnected)
		return;

	if(audio->jack_activated){
		jack_deactivate(audio->jclient);
		audio->jack_activated = false;
		debug("Deactivating client until source ports reappear");
	}
	jack_wait_for_source_ports(audio); /* blocks */

	/* reactivate */
	if (jack_activate (audio->jclient)) {
		perror("cannot activate client");
		return;
	}
	audio->jack_activated = true;
	audio->disconnected = false;
	jack_connect_source_ports(audio);
}
