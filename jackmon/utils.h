/*
 * utils.h
 *
 *  Created on: 19 Aug 2025
 *      Author: chris
 */

#ifndef UTILS_H_
#define UTILS_H_

#define _GNU_SOURCE
#include <stdbool.h>
#include <time.h>
#include <math.h>

#if !defined(__arm__) && !defined(__aarch64__)
typedef double ftype;
#define fpow pow
#define ftan tan
#define flog log10
#define ffabs fabs
#define sqrtff sqrt
#define ftmin -(DBL_MAX)
#else
typedef float ftype;
#define fpow powf
#define ftan tanf
#define flog log10f
#define ffabs fabsf
#define sqrtff sqrtf
#define ftmin -(FLT_MAX)
#endif

static const ftype min_level = fpow(10.0, -130.0/20.0); /* noise below this */

#define to_pow_2(x) ({ \
    unsigned int v = (x); \
    if (v == 0) v = 1; \
    else { while (v & (v - 1)) v += v & -v; } \
    v; })

static inline bool timespec_isset(const struct timespec *ts) {
	return (ts && (ts->tv_sec || ts->tv_nsec));
}

/* wrap up asprintf to exit on no memory - its catastrophic, and this way the code is more readable */
#define Asprintf(...) { if(asprintf(__VA_ARGS__) <= 0) exit(1); }

/* return time for timeout */
static inline void set_timer(struct timespec *ts, int ms) {
	if(clock_gettime(CLOCK_MONOTONIC, ts))
		return;
	ts->tv_nsec += ms%1000 * 1000000L; /* restrict to sub-second magnitude to prevent overflow of nsec var */
	ts->tv_sec += ms/1000 + ts->tv_nsec/1000000000L;
	ts->tv_nsec%=1000000000L;
}

static inline void clear_timer(struct timespec *ts){
	ts->tv_sec=ts->tv_nsec=0;
}

static inline void millisleep(int ms) {
	struct timespec t;
	t.tv_nsec = ms%1000 * 1000000L; /* restrict to sub-second magnitude to prevent overflow of nsec var */
	t.tv_sec = ms/1000 + t.tv_nsec/1000000000L;
	t.tv_nsec%=1000000000L;
	nanosleep(&t,NULL);
}

static inline int timespec_compare(const struct timespec *lhs, const struct timespec *rhs)
{
	if (lhs->tv_sec < rhs->tv_sec)
		return -1;
	if (lhs->tv_sec > rhs->tv_sec)
		return 1;
	if (lhs->tv_nsec < rhs->tv_nsec)
		return -1;
	if (lhs->tv_nsec > rhs->tv_nsec)
		return 1;
	return 0;
}

int timer_poll(struct timespec * ts);

struct systemcall_env {
	char * var;
	char * val;
};

int systemcall(const char * command, const struct systemcall_env * env,  unsigned timeout_ms);
char * gpio_init(int gpio);
int gpio_set(char * path, bool value);
#endif /* UTILS_H_ */
