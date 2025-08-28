/*
 * audio.h
 *
 *  Created on: 20 Aug 2025
 *      Author: chris
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdbool.h>
#include <float.h>
#include <math.h>
#include <jack/jack.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "utils.h"

struct biquad {
	ftype b0, b1, b2;
	ftype a1, a2;
	ftype z1, z2;
	ftype y;
};

struct rms {
	bool en;
	struct biquad f;
	ftype _sum_squared; /* current sum squared */
};

struct peak {
	ftype peak; /* output peak */
	unsigned hold_time;
	struct timespec _hold; /* peak hold timer */
	ftype _peak; /* decaying peak value to comare new samples, and update for when peak hold expires if we don't exceed it */
	unsigned decay_samples; /* number of samples to decay over */
	ftype decay_atten; /* final value to decay to after samples - eg 0.001 */
	ftype _decay; /* constant decay multiplier per sample */
	bool event; /* set here, cleared by background loop */
};

struct clip {
	bool event; /* set here, cleared by background loop */
	unsigned n; /* consecutive clipped samples */
	unsigned threshold; /* number of consecutive samples overloaded to flag clip */
};

/* per channel */
struct chan {
	/* mutex protected data */
	struct rms rms;
	struct peak peak;
	struct clip clip;
	jack_port_t *jport;

	/* double buffered state */
	int pending;
	ftype rms_val;
	ftype peak_val;
	bool clip_event;
};

struct audio {
	/* config items */
	char * name;
	char * server;
	char * config; /* ini style config file */
	char * sources; /* name of source plugin as a regex to determine number of channels */
	char * level_sinks; /* name of sink plugin as a regex to connect source when level is reached */
	bool debug;
	bool noreconnect; /* dont try to reconnect if input link disconnected */
	char * vu_pipe; /* name of file to write VU stream */
	unsigned vu_ms; /* ms poll rate for VU updates- events will update faster */
	unsigned vu_peak_hold_ms; /* ms to hold peak value */
	char * clip_cmd; /* script to run -eg trigger a one-shot LED */
	unsigned clip_ms; /* sets script environment variable CLIP 1 and after duration ms back to 0 */
	unsigned clip_samples; /* number of consecutive clamped samples to trigger clip */
	int clip_gpio; /* positive for active high, negative for active low- sets clip_ms default 200ms if not set */
	/* threshold detector */
	unsigned level_sec; /* time to hold after level collases below threshold */
	char * level_cmd; /* call this with env LEVEL=1 for on, 0 for off- eg pump into a GPIO directly */
	ftype level_thres; /* threshold for setting level/hold */
	int level_gpio; /* sysfs GPIO to control level... negative means active low */

	/* which functions are enabled based on config */
	bool rms_en; /* enable rms calculations */
	bool clip_en; /* enable clipping detection */
	bool vu_pretty;

	/* evaluated */
	jack_client_t * jclient;
	/* from connection */
	ftype samplerate;
	const char ** source_ports; /* list of source ports we are connecting to */
	FILE * _vu; /* file to write VU instead of stdout */
	bool disconnected; /* flag indicating source port disconnected */
	bool jack_activated; /* flag that client is active */
	struct chan * chan; /* pointer to array of stats- one per channel */
	unsigned channels;
	bool started;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	unsigned event; /* event to wake up main thread- eg clip, or peak */

	struct timespec _clip_hold;
	struct timespec _level_hold; /* timer to hold after level trigger */
	const char ** _level_sink_ports; /* array of sink names determined on open */
};

static inline void run_biquad(ftype x, struct biquad * b){
	ftype y = b->b0 * x + b->z1;
	if(y < min_level*min_level) /* below noise floor (mean squared, so ^2)*/
		b->y = b->z1 = b->z2 = 0.0;
	else {
		b->z1 = b->b1 * x - b->a1 * y + b->z2;
		b->z2 = b->b2 * x - b->a2 * y;
	    b->y = y;
	}
}

void rms_init(struct rms * rms, double samplerate);

/* return 1 if RMS value becomes ready */
static inline int rms_run(struct rms * rms, ftype sample) {
	if(!rms->en)
		return 0; /* disabled */
	run_biquad(sample*sample, &rms->f);
	return rms->f.y != 0.0;
}

/* call from background within critical section */

static inline ftype rms_get(struct rms * rms){
	return rms->f.y ? sqrtff(rms->f.y) : 0.0;
}


void peak_init(struct peak * peak, ftype atten, unsigned decay_samples, unsigned hold_ms);

/* track max sample for hold_time ms, with a decay used after timer expires.
 * return 1 if peak changes */
static inline int peak_run(struct peak * peak, ftype sample){
	if(sample < min_level) /* flatten it */
		sample = 0;

	if(!peak->hold_time) {
		if(sample >= peak->peak)
			peak->peak = sample;
		else if (sample > 0)
			peak->peak *= peak->_decay;
		else
			peak->peak = 0.0;
		return 0; /* never trigger events with no hold timer */
	}

	/* use the peak hold timer */
	if(sample >= peak->peak){
		peak->peak = peak->_peak = sample;
		goto peak_detected;
	}

	/* get next peak after peak hold expires */
	if(sample >= peak->_peak)
		peak->_peak = sample;
	else if (sample > 0)
		peak->_peak *= peak->_decay;
	else
		peak->_peak = 0.0;

	if(!timer_poll(&peak->_hold)){
		peak->peak = peak->_peak;
		goto peak_detected;
	}

	return 0;

peak_detected:
	set_timer(&peak->_hold, peak->hold_time);
	peak->event = true;
	return 1;
}

/* grab peak value. return true if peak value updated on hold timer. */
static inline bool peak_get(struct peak * peak, ftype * val){
	*val = peak->peak;
	if(peak->event)
		return false;
	peak->event = false;
	return true;
}

void clip_init(struct clip * clip, unsigned threshold);

/* flag clip if overloaded mode than threshold samples in a row.
 * If we triggered event, return 1, otherwise 0 */
static inline int clip_run(struct clip * clip, ftype sample){
	if (sample >= 0.9999) {
		if(++clip->n >= clip->threshold){
			clip->event = true; /* cleared by background loop */
			return 1;
		}
	} else
		clip->n = 0;
	return 0;
}

/* call from background within critical section - return true if clip tripped on this channel */
static inline bool clip_get(struct clip * clip){
	if(!clip->event)
		return false;
	clip->event = false;
	return true;
}

/* process channel, return accumulated events for post processing */
static inline int audio_chan_run(struct chan * s, ftype sample){
	int ret = 0;
	sample = ffabs(sample); /* only care for magnitude */
	if(s->rms.en)
		ret += rms_run(&s->rms, sample);
	if(s->peak.decay_samples)
		ret += peak_run(&s->peak, sample);
	if(s->clip.threshold)
		ret += clip_run(&s->clip, sample);
	s->pending += ret;
	return ret;
}

extern struct audio gAudio;

/* include jack name in debug messages for journalctl */
static inline void debug(const char* fmt, ...){
	if(!gAudio.debug)
		return;
	fprintf(stderr, "(%s) ", gAudio.name);
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end (args);
}

int audio_init(struct audio * audio);
int audio_poll(struct audio * audio, unsigned period_ms);
void vu_print(struct audio * audio, const char* fmt, ...);
void jack_wait_for_source_ports(struct audio * audio);
void jack_connect_source_ports(struct audio * audio);
void jack_check_source_ports(struct audio * audio);
void vu_print_header(struct audio * audio);
void vu_console_restore(struct audio * audio);
void vu_print_pretty(struct audio * audio, ftype rms, ftype peak, int chan);

#endif /* AUDIO_H_ */
