/** @file jdetect.c
 *
 * @brief Jack client that does level detetion, and writes to a tigger file when threshold exceeded for a number of samples.
 */
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#include "audio.h"
#include "utils.h"

static void printhelp(void);
static void parse_opts(int argc, char *argv[]);
static void parse_config(int argc, char *argv[]);
static bool parseflag(char * val);

/* specify actions
 * -d debug
 * -s source connection regex- finds matching source channels and connects to these
 * -n name of this instance as a jack service
 * -p name of pipe/file to stream {rms peak} pairs in dB, space separated, newline per poll event: use for VU meter
 * -c script to run if we clip- clip detection only enabled in debug mode, or if this script is specified
 *		eg user this to set an LED or write something to a LCD front end.
 * -l threshold in dBfs where if RMS level exceeds this, we consider the source ON
 * -h hold time for threshold detection in ms
 * -E script to run when threshold exceeded, set environment variable LEVEL to 1 or 0
 * -e sink connection regex to map sequentially when threshold is exceeded. disconnect after hold time
 *
 * rms is calculated if -p, and/or -l and -h and -E/-e are specified
 * clip is calculated if debug mode, or -C is specified- this can be a one-shot LED for example.
 * peak is calculated if -p is specified
 */
static void parse_opts(int argc, char *argv[]){
	int o;
	optind = 1;
	while (((o = getopt(argc, argv, "hdNs:e:c:C:G:n:f:t:h:l:E::P:")) != -1)) {
		switch (o) {
		case 'h':
			printhelp();
			break;
		case 'd':
			gAudio.debug = true;
			break;
		case 'N':
			gAudio.noreconnect = true;
			break;
		case 's':
			gAudio.sources=optarg;
			break;
		case 'e':
			gAudio.level_sinks=optarg;
			break;
		case 'C':
			gAudio.clip_cmd=optarg;
			break;
		case 'c':
			gAudio.clip_ms=strtoul(optarg, NULL, 0);
			break;
		case 'G':
			gAudio.clip_gpio=strtol(optarg, NULL, 0);
			break;
		case 'n':
			gAudio.name=optarg;
			break;
		case 'f':
			gAudio.config=optarg;
			break;
		case 't':
			gAudio.level_sec=strtoul(optarg, NULL, 0)*1000; /* seconds to ms */
			break;
		case 'l':
			gAudio.level_thres=fpow(10.0, strtof(optarg, NULL)/20); /* dB to level- should be negative of course */
			break;
		case 'E':
			gAudio.level_cmd=optarg;
			break;
		case 'g':
			gAudio.level_gpio=strtol(optarg, NULL, 0);
			break;
		case 'p':
			gAudio.vu_pipe=optarg;
			break;
		case 'P':
			gAudio.vu_ms=strtoul(optarg, NULL, 0);
			break;
		default:
			printhelp();
			break;
		}
	}
}

static bool parseflag(char * val){
	return !strcasecmp(val, "true") || !strcmp(val, "1");
}

static void parse_config(int argc, char *argv[]){
	parse_opts(argc, argv);

	if(!gAudio.config){ /* no file specified in command line - try default locations */
		if(gAudio.name){ /* client name specified in commandd line */
			Asprintf(&gAudio.config, "/etc/jackmon.d/%s.conf", gAudio.name);
		} else
			gAudio.config = "/etc/jackmon.conf"; /* default unnamed */
	}

	/* try to open config file and parse it */
    FILE *fp = fopen(gAudio.config, "r");
    if (!fp)
    	goto done;

    debug("reading config file %s\n", gAudio.config);

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
    	char *s = line;
    	while (isspace((unsigned char)*s))
    		s++;
        if (*s == '#' || *s == '\n' || *s == '\0')
        	continue;

        char *_key = strtok(s, "=");
        char *val = strtok(NULL, "\n");
        if (!_key || !val)
        	continue;
        char * key = strdup(_key);
        if (!key)
        	continue;

        while (isspace((unsigned char)*(key + strlen(key)-1)))
        	*(key + strlen(key)-1) = 0;

        /* Trim leading whitespace from value */
        while (isspace((unsigned char)*val))
        	val++;

        if (!strcmp(key, "debug"))
        	gAudio.debug = parseflag(val);
        else if (!strcmp(key, "server")){
        	Asprintf(&gAudio.server, "%s", val);
        } else if (!strcmp(key, "name")){
        	Asprintf(&gAudio.name, "%s", val);
        }else if (!strcmp(key, "noreconnect"))
        	gAudio.noreconnect = parseflag(val); /* override noreconnect not set on command line */
        else if (!strcmp(key, "sources")) {
        	Asprintf(&gAudio.sources, "%s", val);
        } else if (!strcmp(key, "level_sinks")) {
        	Asprintf(&gAudio.level_sinks, "%s", val);
        } else if (!strcmp(key, "level_thres")){
        	float l;
        	sscanf(val, "%f", &l);
        	gAudio.level_thres = fpow(10.0, l/20);
        } else if (!strcmp(key, "level_cmd")){
        	Asprintf(&gAudio.level_cmd, "%s", val);
        } else if (!strcmp(key, "level_sec"))
        	gAudio.level_sec = strtoul(val, NULL, 0);
		else if (!strcmp(key, "level_gpio"))
			gAudio.level_gpio = strtol(val, NULL, 0);
        else if (!strcmp(key, "clip_cmd")){
			Asprintf(&gAudio.clip_cmd, "%s", val);
        }else if (!strcmp(key, "clip_ms"))
			gAudio.clip_ms = strtoul(val, NULL, 0);
        else if (!strcmp(key, "clip_samples"))
        	gAudio.clip_samples = strtoul(val, NULL, 0);
        else if (!strcmp(key, "clip_gpio"))
			gAudio.clip_gpio = strtol(val, NULL, 0);
		else if (!strcmp(key, "vu_ms"))
			gAudio.vu_ms = strtoul(val, NULL, 0);
		else if (!strcmp(key, "vu_peak_hold_ms"))
			gAudio.vu_peak_hold_ms = strtoul(val, NULL, 0);
		else if (!strcmp(key, "vu_pipe")){
		    Asprintf(&gAudio.vu_pipe, "%s", val);
		} else if (!strcmp(key, "vu_pretty"))
        	gAudio.vu_pretty = parseflag(val);
        free(key);
    }
    fclose(fp);
done:
	parse_opts(argc, argv); /* command line options take priority so override */
	if(!gAudio.name) /* default if not specified on command line or in config file */
		gAudio.name = "jackmon";
}

int main(int argc, char *argv[]){
	parse_config(argc, argv);

	/* set defaults- analog input port capture for host... in pipewire naming convention */
	if(!gAudio.sources)
		gAudio.sources = "Built-in Audio.*:capture_*";

	/* setup GPIO */
	if((gAudio._level_gpio_file = gpio_init(gAudio.level_gpio)))
		debug("Level GPIO %d %s\n", abs(gAudio.level_gpio), gAudio.level_gpio < 0 ? "Active Low":"Active High");

	/* Set up "vox" to do something when level exceeds threshold */
	if(gAudio.level_sinks || gAudio.level_cmd || gAudio.level_gpio){
		/* lets set up some defaults for RMS detection */
		gAudio.rms_en = true; /* level needs rms */
		if(!gAudio.level_sec)
			gAudio.level_sec = 60; /* 1 minute hold */
		if(!gAudio.level_thres)
			gAudio.level_thres = fpow(10.0, -65.0/20.0); /* -65dB to trigger */
	} else
		gAudio.level_sec = 0; /* use zero timeout to flag we don't use the trigger/hold feature */

	/* VU meterage - uses RMS and peak */
	if(gAudio.vu_pipe && !gAudio.vu_ms)
		gAudio.vu_ms = 50; /* 50ms update rate by default */

	if(gAudio.vu_ms){
		if(!gAudio.vu_peak_hold_ms)
			gAudio.vu_peak_hold_ms = 800; /* peak hold for 800ms */
		gAudio.rms_en = true; /* vu needs rms and peak */
		if(gAudio.vu_pretty)
			vu_print_header(&gAudio);
	}

	/* Handle clipping */
	if(gAudio.clip_cmd || gAudio.debug || gAudio.clip_gpio){
		gAudio.clip_en = true;
		if(!gAudio.clip_samples)
			gAudio.clip_samples = 4;
		if(gAudio.clip_gpio){
			if(!gAudio.clip_ms)
				gAudio.clip_ms = 200; /* default 200ms for LED flash */
			if((gAudio._clip_gpio_file = gpio_init(gAudio.clip_gpio)))
				debug("Clip indicator GPIO %d %s\n", abs(gAudio.clip_gpio), gAudio.clip_gpio < 0 ? "Active Low":"Active High");
		}
	}

	if(!gAudio.rms_en && !gAudio.clip_en) {
		fprintf(stderr, "Empty Configuration- no actions configured\n");
		return 1;
	}

	if(audio_init(&gAudio)){
		debug("Error: Audio init failed\n");
		return 1;
	}

	int threshold_set = -1; /* init */
	int clip_set = -1;
	while(true) {
		int events = audio_poll(&gAudio, gAudio.vu_ms ?:1457); /* poll at VU rate or a prime number reasonable amount */
		if(events < 0){
			debug("polling failed : %s\n", strerror(errno));
			break;
		}
		ftype trigger_level=0;
		bool clip = false;
		bool vuevent = false;
		for (int i=0; i < gAudio.channels; i++){
			struct chan * c = &gAudio.chan[i];
			if(gAudio.vu_ms && (min_level < c->rms_val || min_level < c->peak_val)){
				if(!gAudio.vu_pretty){
					vuevent = true;
					vu_print(&gAudio, "%0.1f %0.1f ", 20*flog(c->rms_val), 20*flog(c->peak_val));
				} else
					vu_print_pretty(&gAudio, 20*flog(c->rms_val), 20*flog(c->peak_val), i);
			}

			if(c->clip_event) {
				clip = true;
				//debug("ch %d clip\n", i+1);
				c->clip_event = false;
			}

			/* capture max trigger level from this block of frames */
			if(gAudio.level_sec && c->rms_val >= gAudio.level_thres && c->rms_val > trigger_level)
				trigger_level = c->rms_val;
		}
		if(vuevent)
			vu_print(&gAudio, "\n");

		if(gAudio.disconnected){ /* reset script timers so we turn stuff off immediately */
			clear_timer(&gAudio._clip_hold);
			clear_timer(&gAudio._level_hold);
		}
		/* clip */
		if(gAudio.clip_en){
			if(clip && !gAudio.disconnected){
				set_timer(&gAudio._clip_hold, gAudio.clip_ms?:200); /* always set timer to limit calls */
				if(clip_set < 1){
					clip_set = 1;
					if(gAudio.clip_cmd){
						debug("Running \"%s\" with env CLIP=1\n", gAudio.clip_cmd);
						static const struct systemcall_env e[] = {{"CLIP" , "1" }, {NULL , NULL }};
						systemcall(gAudio.clip_cmd, e, 100);
					}
					gpio_set(gAudio._clip_gpio_file, true);
				} else if (gAudio.clip_cmd && !gAudio.clip_ms)
					systemcall(gAudio.clip_cmd, NULL, 100); /* one shot- no args */
			} else if (!timer_poll(&gAudio._clip_hold) && clip_set){
				clip_set = 0;
				if (gAudio.clip_ms){
					if(gAudio.clip_cmd){
						debug("Running \"%s\" with env CLIP=0\n", gAudio.clip_cmd);
						static const struct systemcall_env e[] = {{"CLIP" , "0" }, {NULL , NULL }};
						systemcall(gAudio.clip_cmd, e, 100);
					}
					gpio_set(gAudio._clip_gpio_file, false);
				}
			}
		}

		/* run threshold checker */
		if(trigger_level && !gAudio.disconnected){
			set_timer(&gAudio._level_hold, gAudio.level_sec*1000);
			if(threshold_set < 1){
				debug("Level triggered %0.1fdB\n", 20*flog(trigger_level));
				threshold_set = 1;
				if(gAudio._level_sink_ports){ /* connect ports*/
					/* make the connections */
					for(int i = 0; i < gAudio.channels; i++){
						if(!gAudio._level_sink_ports[i])
							break;
						if(!jack_connect(gAudio.jclient, gAudio.source_ports[i], gAudio._level_sink_ports[i]))
							debug("connect %s to %s\n", gAudio.source_ports[i], gAudio._level_sink_ports[i]);
					}
				}

				/* run script */
				if(gAudio.level_cmd){
					debug("Running \"%s\" with env TRIG=1\n", gAudio.level_cmd);
					static const struct systemcall_env e[] = {{"TRIG" , "1" }, {NULL , NULL }};
					systemcall(gAudio.level_cmd, e, 500);
				}

				/* GPIO */
				if(gAudio._level_gpio_file) {
					debug("GPIO %d on\n", abs(gAudio.level_gpio));
					gpio_set(gAudio._level_gpio_file, true);
				}
			}
		} else if (gAudio.level_sec && !timer_poll(&gAudio._level_hold) && threshold_set){ /* -1 (starting) or 1 */
			debug("Trigger %s\n", threshold_set == 1 ? "expired" : "reset");
			threshold_set = 0;
			if(gAudio._level_sink_ports){ /* connect ports*/
				/* make the disconnections */
				for(int i = 0; i < gAudio.channels; i++){
					if(!gAudio._level_sink_ports[i])
						break;
					if(!jack_disconnect(gAudio.jclient, gAudio.source_ports[i], gAudio._level_sink_ports[i]))
						debug("disconnect %s from %s\n", gAudio.source_ports[i], gAudio._level_sink_ports[i]);
				}
			}

			/* run script */
			if(gAudio.level_cmd){
				debug("Running \"%s\" with env TRIG=0\n", gAudio.level_cmd);
				static const struct systemcall_env e[] = {{"TRIG" , "0" }, {NULL , NULL }};
				systemcall(gAudio.level_cmd, e, 500);
			}

			/* GPIO */
			if(gAudio._level_gpio_file){
				debug("GPIO %d off\n", abs(gAudio.level_gpio));
				gpio_set(gAudio._level_gpio_file, false);
			}
		}

		if(gAudio.disconnected) /* wait until we reconnect */
			jack_check_source_ports(&gAudio);
	}

	/* cleanup- kind of redundant */
	debug("Closing\n");
	if(gAudio.vu_pipe)
		fclose(gAudio._vu);
	jack_client_close (gAudio.jclient);
	exit (0);
}

static void printhelp(void) {
	printf("Help:\n"
		"\t-h\tThis help\n"
		"\t-s\tsource connection regex- finds matching source channels and connects to these\n"
		"\t-n\tname of this instance as a jack service\n"
		"\t-f\tconfig file- default is /etc/jackmon.conf if no name set, otherwise /etc/jackmon.d/<instance>.conf\n"
		"\t-p\tname of pipe/file to stream {rms peak} pairs in dB, space separated, newline per poll event: use for VU meter\n"
		"\t-P\tupdate rate of rms values in ms- if set without -p, this will dump to stdout\n"
		"\t-C\tscript to run if we clip- clip detection only enabled in debug mode, or if this script is specified\n"
		"\t-G\tCLIP GPIO to drive LED. Negative number for active low\n"
		"\t-c\tms to call script when clip over like a one-shot, with CLIP env 1 and 0 of this instance as a jack service\n"
		"\t\t\teg user this to set an LED or write something to a LCD front end.\n"
		"\t-l\tthreshold in dBfs where if RMS level exceeds this, we consider the source ON\n"
		"\t-t\thold time for threshold detection in seconds\n"
		"\t-g\tGPIO to dive relay when threshold reached, negative number for active low\n"
		"\t-E\tscript to run when threshold exceeded, set environment variable LEVEL to 1 or 0. Has a 500ms timeout since its blocking\n"
		"\t-e\tsink connection regex to map sequentially when threshold is exceeded. disconnect after hold time\n"
		"\t-N\tDon't try to reconnect if source port connection gets removed\n");
	 exit(0);
}
