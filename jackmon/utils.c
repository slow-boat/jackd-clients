/*
 * utils.c
 *
 *  Created on: 19 Aug 2025
 *      Author: chris
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <wordexp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdarg.h>

#include "utils.h"

/* return 1 if not expired, return 0 if expired (and clear the timer), or if not started */
int timer_poll(struct timespec * ts){
	if(!timespec_isset(ts))
		return 0; /* timer disabled */
	struct timespec now;
	if(!clock_gettime(CLOCK_MONOTONIC, &now) && (timespec_compare(&now, ts)<0))
		return 1;
	ts->tv_sec=ts->tv_nsec=0;
	return 0;
}

/* run a command at path, usual argument parsing.
 * Basic environment variable interpretation wrapped with ${env} is done on command line
 * with environment variables set in an array of struct systemcall_env pairs, with timeout */
int systemcall(const char * command, const struct systemcall_env * env,  unsigned timeout_ms){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    /* Block SIGCHLD so we can wait on it explicitly */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }

    /* child */
    if (pid == 0) {
        /* Unblock SIGCHLD in child to be sure parent gets it when we are done */
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

	    /* Build argv array: [script, args..., NULL] */
	    wordexp_t p;
	    if(wordexp(command, &p, 0))
	    	return -1;
	    size_t argc = p.we_wordc + 1;
	    char **argv = malloc(argc * sizeof(char *));
	    if (!argv) {
	        wordfree(&p);
	        return -1;
	    }
	    argv[p.we_wordc] = NULL;
	    memcpy(argv, p.we_wordv, sizeof(char *)*p.we_wordc);

		while(env && env->var){
			setenv(env->var, env->val, 1);

			/* check if argv contains ${ENV}, and replace */
			char * a;
			if(asprintf(&a, "${%s}", env->var)){
				for(size_t i = 1; i < p.we_wordc; i++){
					char * match = strstr(argv[i], a);
					if(!match)
						continue;
					size_t mp = match-argv[i];
					size_t l = strlen(argv[i]);
					size_t m = strlen(a);
					size_t v = strlen(env->val);
					char * n = calloc(l-m+v+1, 1);
					if(!n)
						continue;
					memcpy(n, argv[i], mp);
					memcpy(n + mp, env->val, v);
					memcpy(n + mp + v, argv[i] + mp + m, l - mp - m);
					argv[i] = n;
				}
			}
			env++;
		}

		/* dont bother to free anything */
        execvp(argv[0], argv);
        return -1;
    }

    /* parent */
    struct timespec ts;
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

    int status;
    siginfo_t si;

    /* Wait for SIGCHLD or timeout */
    int r = sigtimedwait(&mask, &si, &ts);
    if (r == -1) {
        if (errno == EAGAIN) {
            // Timeout
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        } else {
            perror("sigtimedwait");
            return -1;
        }
    }

    /* Child exited → collect exit status */
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid: %s", strerror(errno));
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* return -1 for open error, 1 for write error, 0 for OK */
static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1; /* dont print error- can get permissions error immediately after export if udev hasn't kicked in yet */

    if (write(fd, value, strlen(value)) < 0) {
    	fprintf(stderr, "Failed to write %s: %d: %s\n", path, errno, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

extern struct audio gAudio;
#define SYSFS_GPIO_DIR "/sys/class/gpio"
int gpio_init(struct gpio_info * gpio){
	if(gpio->initialised)
		return 0;

	if(!gpio->gpio)
		return -1; /* no GPIO */
	int err = -1;

	gpio->active_low = gpio < 0;
	gpio->gpio = abs(gpio->gpio);

	/* track GPIO set owner */
	mkdir("/dev/shm/gpio", 0755); /* ignore response */

	if(!gpio->direction_path && asprintf(&gpio->direction_path, SYSFS_GPIO_DIR "/gpio%d/direction", gpio->gpio) < 8)
		goto nomem;

	/* if direction file doesn't exist, we need to export the GPIO */
	struct stat st;
	if(stat(gpio->direction_path, &st)){
		char * s = NULL;
		if(asprintf(&s, "%d", gpio->gpio) < 1)
			goto nomem;
		int export = write_sysfs(SYSFS_GPIO_DIR "/export", s); /* track export to see if we are initialising it here */
		free(s);
		if(export < 0)
			return err;
		fprintf(stderr, "Exported gpio%d for %s\n", gpio->gpio, gpio->name);
	}

	if(write_sysfs(gpio->direction_path, "out") < 0)
		return err;

	if(!gpio->active_low_path && asprintf(&gpio->active_low_path, SYSFS_GPIO_DIR "/gpio%d/active_low", gpio->gpio) < 8)
		return err;
	if(write_sysfs(gpio->active_low_path, gpio->active_low?"1":"0") < 0)
		return err;

	if(!gpio->pidfile && asprintf(&gpio->pidfile, "/dev/shm/gpio/gpio%d", gpio->gpio) < 1)
		goto nomem;

	if(!gpio->value_path && asprintf(&gpio->value_path, SYSFS_GPIO_DIR "/gpio%d/value", abs(gpio->gpio)) < 1)
		goto nomem;

	gpio->initialised = true;
	fprintf(stderr, "Initialised GPIO %s, port %d, Active %s\n", gpio->name, gpio->gpio, gpio->active_low ? "Low" : "High");

	/* try set to off by default if we just exported it- otherwise assume its correct */
	if(gpio_set(gpio, gpio->val) < 0)
		return err;

	return 0;

nomem:
	perror("ERROR: out of memory");
	exit(1);
}

int gpio_set(struct gpio_info * gpio, bool value){
	int err;
	gpio->val = value; /* update in case init fails on udev permissions or export not set up yet, and we do it later */
	if((err = gpio_init(gpio)))
		return err;

	err = -1;
	FILE * pf = fopen(gpio->pidfile, value ? "w":"r");
	if(pf){ /* we've used it before or are setting it to 1*/
		int pid = getpid();
		if(value){
			/* set- write pid to gpio tracking file and set gpio */
			fprintf(pf, "%d", pid);
			err = write_sysfs(gpio->value_path, "1");
		} else { /* read back the pid that last set the gpio, and only clear if it was us */
			int rpid;
			if(!fscanf(pf, "%d", &rpid) || rpid == pid) /* or if we cant read the pid */
				err = write_sysfs(gpio->value_path, "0");
		}
		fclose(pf);
	} else {/* cant open file so just smash the GPIO */
		if(value)
			fprintf(stderr, "Failed to open GPIO track file %s for writing\n", gpio->pidfile);
		err = write_sysfs(gpio->value_path, value?"1":"0");
	}
	return err;
}

/**
 * Create and open a named pipe for read/write, non-blocking,
 * with buffer size set to minimum supported by the system.
 *
 * @param path  Path to the FIFO.
 * @return      File descriptor, or -1 on error.
 */
int fifo_open(const char *path) {
    int fd;

    /* create fifo if not existing already */
    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo");
        return -1;
    }

    /* Open fifo, non-blocking write- with read to keep it open */
    fd = open(path, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    /* Shrink buffer to minimum size PIPE_BUF which is the atomic write size*/
    fcntl(fd, F_SETPIPE_SZ, PIPE_BUF);

    return fd + 1; /* fd can be 0, which we use as "invalid" so add one */
}

int fifo_printf(int fd, const char *fmt, ...) {
	if(fd <= 0)
		return -1;

    va_list ap;
    va_start(ap, fmt);
    int ret = vdprintf(fd - 1, fmt, ap);
	if (ret < 0)
		ret = (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    va_end(ap);
    return ret;
}

void fifo_close(int fd) {
	if(fd > 0)
		close(fd - 1);
	fd = 0;
}
