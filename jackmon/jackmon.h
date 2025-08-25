/*
 * jackmon.h
 *
 *  Created on: 19 Aug 2025
 *      Author: chris
 */

#ifndef JACKMON_H_
#define JACKMON_H_

#include <stdbool.h>
#include <float.h>
#include <jack/jack.h>
#include <pthread.h>

#include "utils.h"

#define debug(fmt, ...) ({ if(g.config.debug) \
	fprintf(stderr, fmt, ## __VA_ARGS__); })


/* OLD */
struct config {
	char *client_name;
	char *server_name;
	unsigned chans; /* process independant channels- don't pipe them all into one and average them... */
	unsigned window_ms; /* window in ms- set to zero and its window of 1 sample */
	unsigned events; /* number of sequential positive window events to trigger >= 1 */
	unsigned hold; /* hold trigger output for this many ms */
	unsigned poll_period; /* polling period in ms */
	ftype level_db;
	char * path; /* path to trigger file */
	char * set; /* when trigger fires, write this value to trigger_path */
	char * clear; /* when trigger hold period is up, write this value to the trigger_path */
	bool debug;
};

struct trigger {
	unsigned mask; /* flag channel triggered */
	ftype max_lvl;	/* highest level seen on any channel since last read */
	int max_chan; /* mark the channel that matches the max_lvl above */
};

struct state {
	struct config config;
	jack_port_t **in; /* array of channels */
	jack_default_audio_sample_t **pin; /* array of buffer pointers since we process by window and want the pointers once per frame */
	unsigned window_sz; /* samples per window */
	unsigned window_idx; /* index into window */
	bool * window_event; /* event counter per window, per channel */
	unsigned * events; /* event counter per channel- accumulate positive result windows */
	ftype level; /* normalised level */
	struct timespec timer; /* event timer */
	struct timespec poll_timer; /* poll reporting timer */
	unsigned poll;

	/* threadsafe */
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	struct trigger trig;
	bool notfirst;
};

#endif /* JACKMON_H_ */
