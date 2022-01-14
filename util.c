/* See LICENSE file for copyright and license details. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"


char buf[1024];


void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;

	if (!(p = calloc(nmemb, size)))
		die("calloc:");
	return p;
}

void
die(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(1);
}

char *
run_command(const char *cmd)
{
    char *p;
    FILE *fp;

    if (!(fp = popen(cmd, "r"))) {
        return NULL;
    }
    p = fgets(buf, sizeof(buf) - 1, fp);
    if (pclose(fp) < 0) {
        return NULL;
    }
    if (!p) {
        return NULL;
    }
    if ((p = strrchr(buf, '\n'))) {
        p[0] = '\0';
    }

    return buf[0] ? buf : NULL;
}

void timespec_set_ms(struct timespec *ts, int32_t ms) {
    ts->tv_sec =  ms/1000;
    ts->tv_nsec = (ms - 1000*ts->tv_sec) * 1000000;
}

void timespec_diff(struct timespec *res, struct timespec *a, struct timespec *b) {
    res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
    res->tv_nsec = a->tv_nsec - b->tv_nsec + (a->tv_nsec < b->tv_nsec) * 1000000000;
}

int32_t timespec_to_ms(struct timespec *ts) {
    return ts->tv_sec*1000 + ts->tv_nsec/1000000;
}
