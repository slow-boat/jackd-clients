/** @file jdetect.c
 *
 * @brief Jack client that does level detetion, and writes to a tigger file when threshold exceeded for a number of samples.
 */
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>

#include "audio.h"
#include "utils.h"

static void printhelp(void);
static void parse_opts(int argc, char *argv[]);
static void parse_config(int argc, char *argv[]);
static bool parseflag(char * val);
static int gpio_init(struct audio * audio);
static int gpio_set(char * path, bool value);

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
	while (((o = getopt(argc, argv, "hdNs:e:c:C:n:f:t:h:l:E::P:")) != -1)) {
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
		else if (!strcmp(key, "vu_ms"))
			gAudio.vu_ms = strtoul(val, NULL, 0);
		else if (!strcmp(key, "vu_pipe")){
		    Asprintf(&gAudio.vu_pipe, "%s", val);
		}
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
	if(gpio_init(&gAudio))
		return 1;

	/* Set up "vox" to do something when level exceeds threshold */
	if(gAudio.level_sinks || gAudio.level_cmd || gAudio.level_gpio){
		/* lets set up some defaults for RMS detection */
		gAudio.rms_en = true;
		if(!gAudio.level_sec)
			gAudio.level_sec = 60; /* 1 minute hold */
		if(!gAudio.level_thres)
			gAudio.level_thres = fpow(10.0, -65.0/20.0); /* -65dB to trigger */
	} else
		gAudio.level_sec = 0; /* use zero timeout to flag we don't use the trigger/hold feature */

	/* VU meterage - uses RMS and peak */
	if(gAudio.vu_ms)
		gAudio.rms_en = gAudio.peak_en = true;

	/* todo Handle clipping */
	if(gAudio.clip_cmd || gAudio.debug)
		gAudio.clip_en = true;

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
				vuevent = true;
				vu_print(&gAudio, "%0.1f %0.1f ", 20*flog(c->rms_val), 20*flog(c->peak_val));
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
		if(gAudio.clip_cmd){
			if(clip && !gAudio.disconnected){
				set_timer(&gAudio._clip_hold, gAudio.clip_ms?:200); /* always set timer to limit calls */
				if(clip_set < 1){
					clip_set = 1;
					debug("Running \"%s\" with env CLIP=1\n", gAudio.clip_cmd);
					static const struct systemcall_env e[] = {{"CLIP" , "1" }, {NULL , NULL }};
					systemcall(gAudio.clip_cmd, e, 100);
				} else if (!gAudio.clip_ms)
					systemcall(gAudio.clip_cmd, NULL, 100); /* one shot- no args */
			} else if (!timer_poll(&gAudio._clip_hold) && clip_set){
				clip_set = 0;
				if (gAudio.clip_ms){
					debug("Running \"%s\" with env CLIP=0\n", gAudio.clip_cmd);
					static const struct systemcall_env e[] = {{"CLIP" , "0" }, {NULL , NULL }};
					systemcall(gAudio.clip_cmd, e, 100);
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
					debug("GPIO %d on\n", gAudio.level_gpio);
					if (gpio_set(gAudio._level_gpio_file, true))
						debug("Failed tp set GPIO\n");
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
				debug("GPIO %d off\n", gAudio.level_gpio);
				if (gpio_set(gAudio._level_gpio_file, false))
					debug("Failed tp clear GPIO\n");
			}
		}

		if(gAudio.disconnected) /* wait until we reconnect */
			jack_check_source_ports(&gAudio);
	}

	/* cleanup- kind of redundant */
	debug("Closing\n");
	if(gAudio.vu_pipe)
		fclose(gAudio.vu);
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
		"\t-c\tms to call script when clip over like a one-shot, with CLIP env 1 and 0 of this instance as a jack service\n"
		"\t\t\teg user this to set an LED or write something to a LCD front end.\n"
		"\t-l\tthreshold in dBfs where if RMS level exceeds this, we consider the source ON\n"
		"\t-t\thold time for threshold detection in seconds\n"
		"\t-E\tscript to run when threshold exceeded, set environment variable LEVEL to 1 or 0. Has a 500ms timeout since its blocking\n"
		"\t-e\tsink connection regex to map sequentially when threshold is exceeded. disconnect after hold time\n"
		"\t-N\tDon't try to reconnect if source port connection gets removed\n");
	 exit(0);
}

static int gpio_init(struct audio * audio){
	if(!audio->level_gpio)
		return 0; /* no GPIO */

	bool active_low = audio->level_gpio < 0;
	audio->level_gpio = abs(audio->level_gpio);

	char * path = NULL;
	if(asprintf(&path, "/sys/class/gpio/gpio%d", audio->level_gpio) < 8)
		goto error;

	struct stat sb;
	int exported = 0;
	FILE * e;
	while (exported < 5 && (stat(path, &sb) || !S_ISDIR(sb.st_mode))) {
		if(exported)
			goto export_wait;
		/* export the gpio, then wait for the directory to appear */
    	if(!((e=fopen("/sys/class/gpio/export", "w"))))
    		goto error;
    	if(!fprintf(e, "%d\n", audio->level_gpio)){
    		fclose(e);
    		goto error;
    	}
    	fclose(e);
export_wait:
    	exported++;
    	millisleep(200);
    }
	if(exported == 5)
		goto error;
	free(path);
	path = NULL;

	if(active_low){
		if(asprintf(&path, "/sys/class/gpio/gpio%d/active_low", audio->level_gpio) < 8)
			goto error;
    	if(!((e=fopen(path, "w"))))
    		goto error;
    	if (fwrite("1", sizeof(char), 1, e) != 1) {
    		fclose(e);
    		fprintf(stderr, "ERROR: Failed to make GPIO %d active low\n", audio->level_gpio);
    		goto error;
    	}
    	fclose(e);
    	free(path);
    	path = NULL;
	}

	/* open the file _level_gpio_file */
	if(asprintf(&path, "/sys/class/gpio/gpio%d/value", audio->level_gpio) < 8)
		goto error;
	if(!((e=fopen(path, "w"))))
		goto error;
	fclose(e);
	debug("Using GPIO %d %s\n", audio->level_gpio, active_low ? "Active Low":"Active High");
	audio->_level_gpio_file = path;
	return 0;

error:
	if(path)
		free(path);
	fprintf(stderr, "ERROR: Failed to export GPIO %d\n", audio->level_gpio);
	return 1;
}

static int gpio_set(char * path, bool value){
	FILE * fp = fopen(path, "w");
    if (fwrite(value?"1":"0", sizeof(char), 1, fp) != 1) {
    	fprintf(stderr, "Error writing %d to GPIO value file %s", value, path);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}
