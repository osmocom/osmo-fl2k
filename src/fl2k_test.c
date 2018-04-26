/*
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 *
 * Copyright (C) 2016-2018 by Steve Markgraf <steve@steve-m.de>
 *
 * based on rtl_test:
 * 
 * Copyright (C) 2012-2014 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2014 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2014 by Michael Tatarinov <kukabu@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef __APPLE__
#include <sys/time.h>
#else
#include <time.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include "getopt/getopt.h"
#endif

#include "osmo-fl2k.h"

#define DEFAULT_SAMPLE_RATE		100000000
#define PPM_DURATION			10
#define PPM_DUMP_TIME			1

struct time_generic
	/* holds all the platform specific values */
{
#ifndef _WIN32
	time_t tv_sec;
	long tv_nsec;
#else
	long tv_sec;
	long tv_nsec;
	int init;
	LARGE_INTEGER frequency;
	LARGE_INTEGER ticks;
#endif
};

static int do_exit = 0;
static fl2k_dev_t *dev = NULL;

static uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
static unsigned int ppm_duration = PPM_DURATION;

static char *buffer;
static int cb_cnt = 0;

void usage(void)
{
	fprintf(stderr,
		"fl2k_test, clock accuracy test for FL2K VGA dongles,\n"
		"also outputs a square wave at fs/2\n\n"
		"Usage:\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-s samplerate (default: 100 MS/s)]\n"
	);
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		fl2k_stop_tx(dev);
		do_exit = 1;
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	fl2k_stop_tx(dev);
	do_exit = 1;
}
#endif

#ifndef _WIN32
static int ppm_gettime(struct time_generic *tg)
{
	int rv = ENOSYS;
	struct timespec ts;

#ifdef __unix__
	rv = clock_gettime(CLOCK_MONOTONIC, &ts);
	tg->tv_sec = ts.tv_sec;
	tg->tv_nsec = ts.tv_nsec;
#elif __APPLE__
	struct timeval tv;

	rv = gettimeofday(&tv, NULL);
	tg->tv_sec = tv.tv_sec;
	tg->tv_nsec = tv.tv_usec * 1000;
#endif
	return rv;
}
#endif

#ifdef _WIN32
static int ppm_gettime(struct time_generic *tg)
{
	int rv;
	int64_t frac;
	if (!tg->init) {
		QueryPerformanceFrequency(&tg->frequency);
		tg->init = 1;
	}
	rv = QueryPerformanceCounter(&tg->ticks);
	tg->tv_sec = tg->ticks.QuadPart / tg->frequency.QuadPart;
	frac = (int64_t)(tg->ticks.QuadPart - (tg->tv_sec * tg->frequency.QuadPart));
	tg->tv_nsec = (long)(frac * 1000000000L / (int64_t)tg->frequency.QuadPart);
	return !rv;
}
#endif

static int ppm_report(uint64_t nsamples, uint64_t interval)
{
	double real_rate, ppm;

	real_rate = nsamples * 1e9 / interval;
	ppm = 1e6 * (real_rate / (double)samp_rate - 1.);
	return (int)round(ppm);
}

static void ppm_test(uint32_t len)
{
	static uint64_t nsamples = 0;
	static uint64_t interval = 0;
	static uint64_t nsamples_total = 0;
	static uint64_t interval_total = 0;
	static struct time_generic ppm_now;
	static struct time_generic ppm_recent;

	static enum {
		PPM_INIT_NO,
		PPM_INIT_DUMP,
		PPM_INIT_RUN
	} ppm_init = PPM_INIT_NO;

	ppm_gettime(&ppm_now);

	if (ppm_init != PPM_INIT_RUN) {
		/*
		* Kyle Keen wrote:
		* PPM_DUMP_TIME throws out the first N seconds of data.
		* The dongle's PPM is usually very bad when first starting up,
		* typically incorrect by more than twice the final value.
		* Discarding the first few seconds allows the value to stabilize much faster.
		*/
		if (ppm_init == PPM_INIT_NO) {
			ppm_recent.tv_sec = ppm_now.tv_sec + PPM_DUMP_TIME;
			ppm_init = PPM_INIT_DUMP;
			return;
		}
		if (ppm_init == PPM_INIT_DUMP && ppm_recent.tv_sec < ppm_now.tv_sec)
			return;
		ppm_recent = ppm_now;
		ppm_init = PPM_INIT_RUN;
		return;
	}
	nsamples += (uint64_t)len;
	interval = (uint64_t)(ppm_now.tv_sec - ppm_recent.tv_sec);
	if (interval < ppm_duration)
		return;
	interval *= 1000000000UL;
	interval += (int64_t)(ppm_now.tv_nsec - ppm_recent.tv_nsec);

	nsamples_total += nsamples;
	interval_total += interval;
	printf("real sample rate: %i current PPM: %i cumulative PPM: %i\n",
		(int)((1000000000UL * nsamples) / interval),
		ppm_report(nsamples, interval),
		ppm_report(nsamples_total, interval_total));
	ppm_recent = ppm_now;
	nsamples = 0;
}

void fl2k_callback(fl2k_data_info_t *data_info)
{
	/* drop first couple of callbacks until everything is settled */
	if (cb_cnt > 20) {
		ppm_test(FL2K_BUF_LEN);
	} else {
		/* as data is repetitive, it only needs to be handed
		 * over until all transfer buffers contain the data */
		data_info->r_buf = buffer;
		cb_cnt++;
	}

}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	int r, opt, i;
	uint32_t dev_index = 0;

	while ((opt = getopt(argc, argv, "d:s:p::h")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			break;
		case 's':
			samp_rate = (uint32_t)atof(optarg);
			break;
		case 'p':
			if (optarg)
				ppm_duration = atoi(optarg);
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	buffer = malloc(FL2K_BUF_LEN);
	if (!buffer)
		goto exit;

	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
	fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		exit(1);
	}

#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	/* initialize buffer with rect signal */
	for (i = 0; i < FL2K_BUF_LEN; i += 2) {
		buffer[i] = 0x00;
		buffer[i+1] = 0xff;
	}

	r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	samp_rate = fl2k_get_sample_rate(dev);

	fprintf(stderr, "Reporting PPM error measurement every %u seconds...\n", ppm_duration);
	fprintf(stderr, "Press ^C after a few minutes.\n");

	while (!do_exit) {
#ifndef _WIN32
		usleep(500000);
#else
		Sleep(0.5);
#endif
	}

	if (do_exit) {
		fprintf(stderr, "\nUser cancel, exiting...\n");
	}

exit:
	fl2k_close(dev);
	free(buffer);

	return 0;
}
