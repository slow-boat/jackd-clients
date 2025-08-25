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

