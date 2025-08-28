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
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s\n", path);
        return -1;
    }
    if (write(fd, value, strlen(value)) < 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

#define SYSFS_GPIO_DIR "/sys/class/gpio"
int gpio_init(int gpio){
	if(!gpio)
		return -1; /* no GPIO */
	int err = -1;

	bool active_low = gpio < 0;
	gpio = abs(gpio);

	char * s = NULL;
	if(asprintf(&s, "%d", gpio) < 1)
		goto nomem;

	int export; /* track export to see if we are initialising it here */
	if(((export = write_sysfs(SYSFS_GPIO_DIR "/export", s))) < 0)
		goto done;
	free(s);

	if(asprintf(&s, SYSFS_GPIO_DIR "/gpio%d/direction", gpio) < 8)
		goto nomem;

	if(write_sysfs(s, "out") < 0)
		goto done;
	free(s);

	if(asprintf(&s, SYSFS_GPIO_DIR "/gpio%d/active_low", gpio) < 8)
		goto nomem;

	if(write_sysfs(s, active_low?"1":"0") < 0)
		goto done;

	/* track GPIO set owner */
	mkdir("/run/gpio", 0755); /* ignore response */

	/* try set to off by default if we just exported it- otherwise assume its correct */
	if(!export && gpio_set(gpio, 0) < 0)
		goto done;

	err = 0;

done:
	if(s)
		free(s);
	return err;

nomem:
	perror("ERROR: out of memory");
	exit(1);
}

int gpio_set(int gpio, bool value){
	char * path;	/* get path to value file */
	if(asprintf(&path, SYSFS_GPIO_DIR "/gpio%d/value", abs(gpio)) < 1)
		goto nomem;

	char * pidfile;
	if(asprintf(&path, "/run/gpio/gpio%d", abs(gpio)) < 1)
		goto nomem;
	bool retry = 0;

try_export:;
	int err = -1;
	FILE * pf = fopen(pidfile, value ? "w":"r");
	if(pf){ /* we've used it before or are setting it to 1*/
		int pid = getpid();
		if(value){
			/* set- write pid to gpio tracking file and set gpio */
			fprintf(pf, "%d", pid);
			err = write_sysfs(path, "1");
		} else { /* read back the pid that last set the gpio, and only clear if it was us */
			int rpid;
			if(!fscanf(pf, "%d", &rpid) || rpid == pid) /* or if we cant read the pid */
				err = write_sysfs(path, "0");
		}
		fclose(pf);
	} else /* cant open file so just smash the GPIO */
		err = write_sysfs(path, value?"1":"0");

	if(err < 0 && !retry){ /* try to export it */
		retry = true;
		if((err=gpio_init(gpio)))
			goto done;
		goto try_export;
	}

done:
	if (path)
		free(path);
	if(pidfile)
		free(pidfile);

	return err;
nomem:
	perror("ERROR: out of memory");
	exit(1);
}

