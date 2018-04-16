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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* for TCP_NODELAY */
#include <fcntl.h>
#else
#include <winsock2.h>
#include "getopt/getopt.h"
#endif

#include "osmo-fl2k.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")

typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#endif

static SOCKET s;
static fl2k_dev_t *dev = NULL;
static volatile int do_exit = 0;
static volatile int connected = 0;
static char *txbuf = NULL;
static fd_set readfds;
static SOCKET sock;

void usage(void)
{
	fprintf(stderr,
		"fl2k_tcp, a spectrum client for FL2K VGA dongles\n\n"
		"Usage:\t[-a server address]\n"
		"\t[-d device index (default: 0)]\n"
		"\t[-p port (default: 1234)]\n"
		"\t[-s samplerate in Hz (default: 100 MS/s)]\n"
		"\t[-b number of buffers (default: 4)]\n"
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
	do_exit = 1;
	fl2k_stop_tx(dev);
}
#endif

void fl2k_callback(fl2k_data_info_t *data_info)
{
	int left = FL2K_BUF_LEN;
	int received;
	int r;
	struct timeval tv = { 1, 0 };

	if (!connected)
		return;

	data_info->sampletype_signed = 1;
	data_info->r_buf = txbuf;

	while (!do_exit && (left > 0)) {
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(sock + 1, &readfds, NULL, NULL, &tv);

		if (r) {
			received = recv(sock, txbuf + (FL2K_BUF_LEN - left), left, 0);
			left -= received;
		}
	}
}

int main(int argc, char **argv)
{
	int r, opt, i;
	char *addr = "127.0.0.1";
	int port = 1234;
	uint32_t samp_rate = 100000000;
	struct sockaddr_in local, remote;
	uint32_t buf_num = 0;
	int dev_index = 0;
	int dev_given = 0;
	int flag = 1;

#ifdef _WIN32
	WSADATA wsd;
	i = WSAStartup(MAKEWORD(2,2), &wsd);
#else
	struct sigaction sigact, sigign;
#endif

	while ((opt = getopt(argc, argv, "d:s:a:p:b:")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			dev_given = 1;
			break;
		case 's':
			samp_rate = (uint32_t)atof(optarg);
			break;
		case 'a':
			addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (dev_index < 0) {
		exit(1);
	}

	txbuf = malloc(FL2K_BUF_LEN);

	if (!txbuf) {
		fprintf(stderr, "malloc error!\n");
		exit(1);
	}

	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		exit(1);
	}

	r = fl2k_start_tx(dev, fl2k_callback, NULL, buf_num);

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
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	memset(&remote, 0, sizeof(remote));

	remote.sin_family = AF_INET;
	remote.sin_port = htons(port);
	remote.sin_addr.s_addr = inet_addr(addr);

	fprintf(stderr, "Connecting to %s:%d...\n", addr, port);
	while (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
#ifndef _WIN32
		usleep(500000);
#else
		Sleep(0.5);
#endif
		if (do_exit)
			goto out;
	}

	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,sizeof(flag));
	fprintf(stderr, "Connected\n");
	connected = 1;

	while (!do_exit) {
#ifndef _WIN32
		usleep(500000);
#else
		Sleep(0.5);
#endif
	}

out:
	free(txbuf);
	fl2k_close(dev);
	closesocket(s);
#ifdef _WIN32
	WSACleanup();
#endif

	return 0;
}
