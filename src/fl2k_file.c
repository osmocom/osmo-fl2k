/*
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 *
 * Copyright (C) 2016-2018 by Steve Markgraf <steve@steve-m.de>
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

#ifndef _WIN32
#include <unistd.h>
#define sleep_ms(ms)	usleep(ms*1000)
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "getopt/getopt.h"
#define sleep_ms(ms)	Sleep(ms)
#endif

#include "osmo-fl2k.h"

static fl2k_dev_t *dev = NULL;

static volatile int do_exit = 0;
static volatile int repeat = 1;
FILE *file;
char *txbuf = NULL;

void usage(void)
{
	fprintf(stderr,
		"fl2k_file, a sample player for FL2K VGA dongles\n\n"
		"Usage:\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-r repeat file (default: 1)]\n"
		"\t[-s samplerate (default: 100 MS/s)]\n"
		"\tfilename (use '-' to read from stdin)\n\n"
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

void fl2k_callback(fl2k_data_info_t *data_info)
{
	int r, left = FL2K_BUF_LEN;
	static uint32_t repeat_cnt = 0;

	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}

	data_info->sampletype_signed = 1;
	data_info->r_buf = txbuf;

	while (!do_exit && (left > 0)) {
		r = fread(txbuf + (FL2K_BUF_LEN - left), 1, left, file);

		if (ferror(file))
			fprintf(stderr, "File Error\n");

		if (feof(file)) {
			if (repeat && (r > 0)) {
				repeat_cnt++;
				fprintf(stderr, "repeat %d\n", repeat_cnt);
				rewind(file);
			} else {
				fl2k_stop_tx(dev);
				do_exit = 1;
			}
		}

		if (r > 0)
			left -= r;
	}
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact, sigign;
#endif
	int r, opt, i;
	uint32_t samp_rate = 100000000;
	uint32_t buf_num = 0;
	int dev_index = 0;
	void *status;
	char *filename = NULL;

	while ((opt = getopt(argc, argv, "d:r:s:")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			break;
		case 'r':
			repeat = (int)atoi(optarg);
			break;
		case 's':
			samp_rate = (uint32_t)atof(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (argc <= optind)
		usage();
	else
		filename = argv[optind];

	if (dev_index < 0)
		exit(1);

	if (strcmp(filename, "-") == 0) { /* Read samples from stdin */
		file = stdin;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file = fopen(filename, "rb");
		if (!file) {
			fprintf(stderr, "Failed to open %s\n", filename);
			return -ENOENT;
		}
	}

	txbuf = malloc(FL2K_BUF_LEN);
	if (!txbuf) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}

	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");


#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	while (!do_exit)
		sleep_ms(500);

	fl2k_close(dev);

out:
	if (txbuf)
		free(txbuf);

	if (file && (file != stdin))
		fclose(file);

	return 0;
}
