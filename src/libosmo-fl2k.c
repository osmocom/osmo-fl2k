/*
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 *
 * Copyright (C) 2016-2020 by Steve Markgraf <steve@steve-m.de>
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
#include <libusb.h>
#include <pthread.h>

#ifndef _WIN32
#include <unistd.h>
#define sleep_ms(ms)	usleep(ms*1000)
#else
#include <windows.h>
#define sleep_ms(ms)	Sleep(ms)
#endif

/*
 * All libusb callback functions should be marked with the LIBUSB_CALL macro
 * to ensure that they are compiled with the same calling convention as libusb.
 *
 * If the macro isn't available in older libusb versions, we simply define it.
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

/* libusb < 1.0.9 doesn't have libusb_handle_events_timeout_completed */
#ifndef HAVE_LIBUSB_HANDLE_EVENTS_TIMEOUT_COMPLETED
#define libusb_handle_events_timeout_completed(ctx, tv, c) \
	libusb_handle_events_timeout(ctx, tv)
#endif

#include "osmo-fl2k.h"

enum fl2k_async_status {
	FL2K_INACTIVE = 0,
	FL2K_CANCELING,
	FL2K_RUNNING
};

typedef enum fl2k_buf_state {
	BUF_EMPTY = 0,
	BUF_SUBMITTED,
	BUF_FILLED,
} fl2k_buf_state_t;

typedef struct fl2k_xfer_info {
	fl2k_dev_t *dev;
	uint64_t seq;
	fl2k_buf_state_t state;
} fl2k_xfer_info_t;

struct fl2k_dev {
	libusb_context *ctx;
	struct libusb_device_handle *devh;
	uint32_t xfer_num;
	uint32_t xfer_buf_num;
	uint32_t xfer_buf_len;
	struct libusb_transfer **xfer;
	unsigned char **xfer_buf;

	fl2k_xfer_info_t *xfer_info;

	fl2k_tx_cb_t cb;
	void *cb_ctx;
	enum fl2k_async_status async_status;
	int async_cancel;

	int use_zerocopy;
	int terminate;

	/* thread related */
	pthread_t usb_worker_thread;
	pthread_t sample_worker_thread;
	pthread_mutex_t buf_mutex;
	pthread_cond_t buf_cond;

	double rate; /* Hz */

	/* status */
	int dev_lost;
	int driver_active;
	uint32_t underflow_cnt;
};

typedef struct fl2k_dongle {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} fl2k_dongle_t;

static fl2k_dongle_t known_devices[] = {
	{ 0x1d5c, 0x2000, "FL2000DX OEM" },
};

#define DEFAULT_BUF_NUMBER	4

#define CTRL_IN		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT	(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)
#define CTRL_TIMEOUT	300
#define BULK_TIMEOUT	0

static int fl2k_read_reg(fl2k_dev_t *dev, uint16_t reg, uint32_t *val)
{
	int r;
	uint8_t data[4];

	if (!dev || !val)
		return FL2K_ERROR_INVALID_PARAM;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0x40,
				    0, reg, data, 4, CTRL_TIMEOUT);

	if (r < 4)
		fprintf(stderr, "Error, short read from register!\n");

	*val = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];

	return r;
}

static int fl2k_write_reg(fl2k_dev_t *dev, uint16_t reg, uint32_t val)
{
	uint8_t data[4];

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	data[0] = val & 0xff;
	data[1] = (val >> 8) & 0xff;
	data[2] = (val >> 16) & 0xff;
	data[3] = (val >> 24) & 0xff;

	return libusb_control_transfer(dev->devh, CTRL_OUT, 0x41,
				       0, reg, data, 4, CTRL_TIMEOUT);
}

int fl2k_init_device(fl2k_dev_t *dev)
{
	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	/* initialization */
	fl2k_write_reg(dev, 0x8020, 0xdf0000cc);

	/* set DAC freq to lowest value possible to avoid
	 * underrun during init */
	fl2k_write_reg(dev, 0x802c, 0x00416f3f);

	fl2k_write_reg(dev, 0x8048, 0x7ffb8004);
	fl2k_write_reg(dev, 0x803c, 0xd701004d);
	fl2k_write_reg(dev, 0x8004, 0x0000031c);
	fl2k_write_reg(dev, 0x8004, 0x0010039d);
	fl2k_write_reg(dev, 0x8008, 0x07800898);

	fl2k_write_reg(dev, 0x801c, 0x00000000);
	fl2k_write_reg(dev, 0x0070, 0x04186085);

	/* blanking magic */
	fl2k_write_reg(dev, 0x8008, 0xfeff0780);
	fl2k_write_reg(dev, 0x800c, 0x0000f001);

	/* VSYNC magic */
	fl2k_write_reg(dev, 0x8010, 0x0400042a);
	fl2k_write_reg(dev, 0x8014, 0x0010002d);

	fl2k_write_reg(dev, 0x8004, 0x00000002);

	return 0;
}

int fl2k_deinit_device(fl2k_dev_t *dev)
{
	int r = 0;

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	/* TODO, power down DACs, PLL, put device in reset */

	return r;
}

static double fl2k_reg_to_freq(uint32_t reg)
{
	double sample_clock, offset, offs_div;
	uint32_t pll_clock = 160000000;
	uint8_t div = reg & 0x3f;
	uint8_t out_div = (reg >> 8) & 0xf;
	uint8_t frac = (reg >> 16) & 0xf;
	uint8_t mult = (reg >> 20) & 0xf;

	sample_clock = (pll_clock * mult) / (uint32_t)div;
	offs_div = (pll_clock / 5.0f ) * mult;
	offset = ((double)sample_clock/(offs_div/2)) * 1000000.0f;
	sample_clock += (uint32_t)offset * frac;
	sample_clock /= out_div;

//	fprintf(stderr, "div: %d\tod: %d\tfrac: %d\tmult %d\tclock: %f\treg "
//			"%08x\n", div, out_div, frac, mult, sample_clock, reg);

	return sample_clock;
}

int fl2k_set_sample_rate(fl2k_dev_t *dev, uint32_t target_freq)
{
	double sample_clock, error, last_error = 1e20f;
	uint32_t reg = 0, result_reg = 0;
	uint8_t div, mult, frac, out_div;

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	/* Output divider (accepts value 1-15) 
	 * works, but adds lots of phase noise, so do not use it */
	out_div = 1;

	/* Observation: PLL multiplier of 7 works, but has more phase
	 * noise. Prefer multiplier 6 and 5 */
	for (mult = 6; mult >= 3; mult--) {
		for (div = 63; div > 1; div--) {
			for (frac = 1; frac <= 15; frac++) {
				reg =  (mult << 20) | (frac << 16) |
				       (0x60 << 8) | (out_div << 8) | div;

				sample_clock = fl2k_reg_to_freq(reg);
				error = sample_clock - (double)target_freq;

				/* Keep closest match */
				if (fabs(error) < last_error) {
					result_reg = reg;
					last_error = fabs(error);
				}
			}
		}
	}

	sample_clock = fl2k_reg_to_freq(result_reg);
	error = sample_clock - (double)target_freq;
	dev->rate = sample_clock;

	if (fabs(error) > 1)
		fprintf(stderr, "Requested sample rate %d not possible, using"
		                " %f, error is %f\n", target_freq, sample_clock, error); 

	return fl2k_write_reg(dev, 0x802c, result_reg);
}

uint32_t fl2k_get_sample_rate(fl2k_dev_t *dev)
{
	if (!dev)
		return 0;

	return (uint32_t)dev->rate;
}

static fl2k_dongle_t *find_known_device(uint16_t vid, uint16_t pid)
{
	unsigned int i;
	fl2k_dongle_t *device = NULL;

	for (i = 0; i < sizeof(known_devices)/sizeof(fl2k_dongle_t); i++ ) {
		if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
			device = &known_devices[i];
			break;
		}
	}

	return device;
}

uint32_t fl2k_get_device_count(void)
{
	int i,r;
	libusb_context *ctx;
	libusb_device **list;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	ssize_t cnt;

	r = libusb_init(&ctx);
	if (r < 0)
		return 0;

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return device_count;
}

const char *fl2k_get_device_name(uint32_t index)
{
	int i,r;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	fl2k_dongle_t *device = NULL;
	uint32_t device_count = 0;
	ssize_t cnt;

	r = libusb_init(&ctx);
	if (r < 0)
		return "";

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			device_count++;

			if (index == device_count - 1)
				break;
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	if (device)
		return device->name;
	else
		return "";
}

int fl2k_open(fl2k_dev_t **out_dev, uint32_t index)
{
	int r;
	int i;
	libusb_device **list;
	fl2k_dev_t *dev = NULL;
	libusb_device *device = NULL;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	uint8_t reg;
	ssize_t cnt;

	dev = malloc(sizeof(fl2k_dev_t));
	if (NULL == dev)
		return -ENOMEM;

	memset(dev, 0, sizeof(fl2k_dev_t));

	r = libusb_init(&dev->ctx);
	if(r < 0){
		free(dev);
		return -1;
	}

#if LIBUSB_API_VERSION >= 0x01000106
	libusb_set_option(dev->ctx, LIBUSB_OPTION_LOG_LEVEL, 3);
#else
	libusb_set_debug(dev->ctx, 3);
#endif

	dev->dev_lost = 1;

	cnt = libusb_get_device_list(dev->ctx, &list);

	for (i = 0; i < cnt; i++) {
		device = list[i];

		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct)) {
			device_count++;
		}

		if (index == device_count - 1)
			break;

		device = NULL;
	}

	if (!device) {
		r = -1;
		goto err;
	}

	r = libusb_open(device, &dev->devh);
	libusb_free_device_list(list, 1);
	if (r < 0) {
		fprintf(stderr, "usb_open error %d\n", r);
		if(r == LIBUSB_ERROR_ACCESS)
			fprintf(stderr, "Please fix the device permissions, e.g. "
			"by installing the udev rules file\n");
		goto err;
	}

	/* If the adapter has an SPI flash for the Windows driver, we
	 * need to detach the USB mass storage driver first in order to
	 * open the device */
	if (libusb_kernel_driver_active(dev->devh, 3) == 1) {
		fprintf(stderr, "Kernel mass storage driver is attached, "
				"detaching driver. This may take more than"
				" 10 seconds!\n");
		r = libusb_detach_kernel_driver(dev->devh, 3);
		if (r < 0) {
			fprintf(stderr, "Failed to detach mass storage "
					"driver: %d\n", r);
			goto err;
		}
	}

	r = libusb_claim_interface(dev->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface 0 error %d\n", r);
		goto err;
	}

	r = libusb_set_interface_alt_setting(dev->devh, 0, 1);
	if (r < 0) {
		fprintf(stderr, "Failed to switch interface 0 to "
				"altsetting 1, trying to use interface 1\n");

		r = libusb_claim_interface(dev->devh, 1);
		if (r < 0) {
			fprintf(stderr, "Could not claim interface 1: %d\n", r);
		}
	}

	r = fl2k_init_device(dev);
	if (r < 0)
		goto err;

	dev->dev_lost = 0;

found:
	*out_dev = dev;

	return 0;
err:
	if (dev) {
		if (dev->ctx)
			libusb_exit(dev->ctx);

		free(dev);
	}

	return r;
}

int fl2k_close(fl2k_dev_t *dev)
{
	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	if(!dev->dev_lost) {
		/* block until all async operations have been completed (if any) */
		while (FL2K_INACTIVE != dev->async_status)
			sleep_ms(100);

		fl2k_deinit_device(dev);
	}

	libusb_release_interface(dev->devh, 0);
	libusb_close(dev->devh);
	libusb_exit(dev->ctx);

	free(dev);

	return 0;
}

static struct libusb_transfer *fl2k_get_next_xfer(fl2k_dev_t *dev,
					          fl2k_buf_state_t state)
{
	unsigned int i;
	int next_buf = -1;
	uint64_t next_seq = 0;
	fl2k_xfer_info_t *xfer_info;

	for (i = 0; i < dev->xfer_buf_num; i++) {
		xfer_info = (fl2k_xfer_info_t *)dev->xfer[i]->user_data;
		if (!xfer_info)
			continue;

		if (xfer_info->state == state) {
			if (state == BUF_EMPTY) {
				return dev->xfer[i];
			} else if ((xfer_info->seq < next_seq) || next_buf < 0) {
				next_seq = xfer_info->seq;
				next_buf = i;
			}
		}
	}

	if ((state == BUF_FILLED) && (next_buf >= 0))
		return dev->xfer[next_buf];
	else
		return NULL;
}

static void LIBUSB_CALL _libusb_callback(struct libusb_transfer *xfer)
{
	fl2k_xfer_info_t *xfer_info = (fl2k_xfer_info_t *)xfer->user_data;
	fl2k_xfer_info_t *next_xfer_info;
	fl2k_dev_t *dev = (fl2k_dev_t *)xfer_info->dev;
	struct libusb_transfer *next_xfer = NULL;
	int r = 0;

	if (LIBUSB_TRANSFER_COMPLETED == xfer->status) {
		/* resubmit transfer */
		if (FL2K_RUNNING == dev->async_status) {
			/* get next transfer */
			next_xfer = fl2k_get_next_xfer(dev, BUF_FILLED);

			if (next_xfer) {
				next_xfer_info = (fl2k_xfer_info_t *) next_xfer->user_data;

				/* Submit next filled transfer */
				next_xfer_info->state = BUF_SUBMITTED;
				r = libusb_submit_transfer(next_xfer);
				xfer_info->state = BUF_EMPTY;
				pthread_cond_signal(&dev->buf_cond);
			} else {
				/* We need to re-submit the transfer
				 * in any case, as otherwise the device
				 * stops to output data and hangs
				 * (happens only in the hacked 'gapless'
				 * mode without HSYNC and VSYNC)  */
				r = libusb_submit_transfer(xfer);
				pthread_cond_signal(&dev->buf_cond);
				dev->underflow_cnt++;
			}
		}
	}

	if (((LIBUSB_TRANSFER_CANCELLED != xfer->status) &&
	     (LIBUSB_TRANSFER_COMPLETED != xfer->status)) ||
	     (r == LIBUSB_ERROR_NO_DEVICE)) {
			dev->dev_lost = 1;
			fl2k_stop_tx(dev);
			pthread_cond_signal(&dev->buf_cond);
			fprintf(stderr, "cb transfer status: %d, submit "
				"transfer %d, canceling...\n", xfer->status, r);
	}
}

static int fl2k_alloc_submit_transfers(fl2k_dev_t *dev)
{
	unsigned int i;
	int r = 0;
	const char *incr_usbfs = "Please increase your allowed usbfs buffer"
				 " size with the following command:\n"
				 "echo 0 > /sys/module/usbcore/parameters/"
				 "usbfs_memory_mb\n";

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	dev->xfer = malloc(dev->xfer_buf_num * sizeof(struct libusb_transfer *));

	for (i = 0; i < dev->xfer_buf_num; ++i)
		dev->xfer[i] = libusb_alloc_transfer(0);

	dev->xfer_buf = malloc(dev->xfer_buf_num * sizeof(unsigned char *));
	memset(dev->xfer_buf, 0, dev->xfer_buf_num * sizeof(unsigned char *));

	dev->xfer_info = malloc(dev->xfer_buf_num * sizeof(fl2k_xfer_info_t));
	memset(dev->xfer_info, 0, dev->xfer_buf_num * sizeof(fl2k_xfer_info_t));

#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
	fprintf(stderr, "Allocating %d zero-copy buffers\n", dev->xfer_buf_num);

	dev->use_zerocopy = 1;
	for (i = 0; i < dev->xfer_buf_num; ++i) {
		dev->xfer_buf[i] = libusb_dev_mem_alloc(dev->devh, dev->xfer_buf_len);

		if (dev->xfer_buf[i]) {
			/* Check if Kernel usbfs mmap() bug is present: if the
			 * mapping is correct, the buffers point to memory that
			 * was memset to 0 by the Kernel, otherwise, they point
			 * to random memory. We check if the buffers are zeroed
			 * and otherwise fall back to buffers in userspace.
			 */
			if (dev->xfer_buf[i][0] || memcmp(dev->xfer_buf[i],
							  dev->xfer_buf[i] + 1,
							  dev->xfer_buf_len - 1)) {
				fprintf(stderr, "Detected Kernel usbfs mmap() "
						"bug, falling back to buffers "
						"in userspace\n");
				dev->use_zerocopy = 0;
				break;
			}
		} else {
			fprintf(stderr, "Failed to allocate zero-copy "
					"buffer for transfer %d\n%sFalling "
					"back to buffers in userspace\n",
					i, incr_usbfs);
			dev->use_zerocopy = 0;
			break;
		}
	}

	/* zero-copy buffer allocation failed (partially or completely)
	 * we need to free the buffers again if already allocated */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i])
				libusb_dev_mem_free(dev->devh,
						    dev->xfer_buf[i],
						    dev->xfer_buf_len);
		}
	}
#endif

	/* no zero-copy available, allocate buffers in userspace */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			dev->xfer_buf[i] = malloc(dev->xfer_buf_len);

			if (!dev->xfer_buf[i])
				return FL2K_ERROR_NO_MEM;
		}
	}

	/* fill transfers */
	for (i = 0; i < dev->xfer_buf_num; ++i) {
		libusb_fill_bulk_transfer(dev->xfer[i],
					  dev->devh,
					  0x01,
					  dev->xfer_buf[i],
					  dev->xfer_buf_len,
					  _libusb_callback,
					  &dev->xfer_info[i],
					  0);

		dev->xfer_info[i].dev = dev;
		dev->xfer_info[i].state = BUF_EMPTY;

		/* if we allocate the memory through the Kernel, it is
		 * already cleared */
		if (!dev->use_zerocopy)
			memset(dev->xfer_buf[i], 0, dev->xfer_buf_len);
	}

	/* submit transfers */
	for (i = 0; i < dev->xfer_num; ++i) {
		r = libusb_submit_transfer(dev->xfer[i]);
		dev->xfer_info[i].state = BUF_SUBMITTED;

		if (r < 0) {
			fprintf(stderr, "Failed to submit transfer %i\n%s",
					i, incr_usbfs);
			break;
		}
	}

	return 0;
}

static int _fl2k_free_async_buffers(fl2k_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	if (dev->xfer) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer[i]) {
				libusb_free_transfer(dev->xfer[i]);
			}
		}

		free(dev->xfer);
		dev->xfer = NULL;
	}

	if (dev->xfer_buf) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i]) {
				if (dev->use_zerocopy) {
#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
					libusb_dev_mem_free(dev->devh,
							    dev->xfer_buf[i],
							    dev->xfer_buf_len);
#endif
				} else {
					free(dev->xfer_buf[i]);
				}
			}
		}

		free(dev->xfer_buf);
		dev->xfer_buf = NULL;
	}

	return 0;
}

static void *fl2k_usb_worker(void *arg)
{
	fl2k_dev_t *dev = (fl2k_dev_t *)arg;
	struct timeval tv = { 1, 0 };
	struct timeval zerotv = { 0, 0 };
	enum fl2k_async_status next_status = FL2K_INACTIVE;
	int r = 0;
	unsigned int i;

	while (FL2K_RUNNING == dev->async_status) {
		r = libusb_handle_events_timeout_completed(dev->ctx, &tv,
							   &dev->async_cancel);
	}

	while (FL2K_INACTIVE != dev->async_status) {
		r = libusb_handle_events_timeout_completed(dev->ctx, &tv,
							   &dev->async_cancel);
		if (r < 0) {
			/*fprintf(stderr, "handle_events returned: %d\n", r);*/
			if (r == LIBUSB_ERROR_INTERRUPTED) /* stray signal */
				continue;
			break;
		}

		if (FL2K_CANCELING == dev->async_status) {
			next_status = FL2K_INACTIVE;

			if (!dev->xfer)
				break;

			for (i = 0; i < dev->xfer_buf_num; ++i) {
				if (!dev->xfer[i])
					continue;

				if (LIBUSB_TRANSFER_CANCELLED !=
						dev->xfer[i]->status) {
					r = libusb_cancel_transfer(dev->xfer[i]);
					/* handle events after canceling
					 * to allow transfer status to
					 * propagate */
					libusb_handle_events_timeout_completed(dev->ctx,
									       &zerotv, NULL);
					if (r < 0)
						continue;

					next_status = FL2K_CANCELING;
				}
			}

			if (dev->dev_lost || FL2K_INACTIVE == next_status) {
				/* handle any events that still need to
				 * be handled before exiting after we
				 * just cancelled all transfers */
				libusb_handle_events_timeout_completed(dev->ctx,
								       &zerotv, NULL);
				break;
			}
		}
	}

	/* wake up sample worker */
	pthread_cond_signal(&dev->buf_cond);

	/* wait for sample worker thread to finish before freeing buffers */
	pthread_join(dev->sample_worker_thread, NULL);  
	_fl2k_free_async_buffers(dev);
	dev->async_status = next_status;

	pthread_exit(NULL);
}

/* Buffer format conversion functions for R, G, B DACs */
static inline void fl2k_convert_r(char *out,
				  char *in,
				  uint32_t len,
				  uint8_t offset)
{
	unsigned int i, j = 0;

	if (!in || !out)
		return;

	for (i = 0; i < len; i += 24) {
		out[i+ 6] = in[j++] + offset;
		out[i+ 1] = in[j++] + offset;
		out[i+12] = in[j++] + offset;
		out[i+15] = in[j++] + offset;
		out[i+10] = in[j++] + offset;
		out[i+21] = in[j++] + offset;
		out[i+16] = in[j++] + offset;
		out[i+19] = in[j++] + offset;
	}
}

static inline void fl2k_convert_g(char *out,
				  char *in,
				  uint32_t len,
				  uint8_t offset)
{
	unsigned int i, j = 0;

	if (!in || !out)
		return;

	for (i = 0; i < len; i += 24) {
		out[i+ 5] = in[j++] + offset;
		out[i+ 0] = in[j++] + offset;
		out[i+ 3] = in[j++] + offset;
		out[i+14] = in[j++] + offset;
		out[i+ 9] = in[j++] + offset;
		out[i+20] = in[j++] + offset;
		out[i+23] = in[j++] + offset;
		out[i+18] = in[j++] + offset;
	}
}

static inline void fl2k_convert_b(char *out,
				  char *in,
				  uint32_t len,
				  uint8_t offset)
{
	unsigned int i, j = 0;

	if (!in || !out)
		return;

	for (i = 0; i < len; i += 24) {
		out[i+ 4] = in[j++] + offset;
		out[i+ 7] = in[j++] + offset;
		out[i+ 2] = in[j++] + offset;
		out[i+13] = in[j++] + offset;
		out[i+ 8] = in[j++] + offset;
		out[i+11] = in[j++] + offset;
		out[i+22] = in[j++] + offset;
		out[i+17] = in[j++] + offset;
	}
}

static void *fl2k_sample_worker(void *arg)
{
	int r = 0;
	unsigned int i, j;
	fl2k_dev_t *dev = (fl2k_dev_t *)arg;
	fl2k_xfer_info_t *xfer_info = NULL;
	struct libusb_transfer *xfer = NULL;
	char *out_buf = NULL;
	fl2k_data_info_t data_info;
	uint32_t underflows = 0;
	uint64_t buf_cnt = 0;

	while (FL2K_RUNNING == dev->async_status) {
		memset(&data_info, 0, sizeof(fl2k_data_info_t));

		data_info.len = FL2K_BUF_LEN;
		data_info.underflow_cnt = dev->underflow_cnt;
		data_info.ctx = dev->cb_ctx;

		if (dev->underflow_cnt > underflows) {
			fprintf(stderr, "Underflow! Skipped %d buffers\n",
					dev->underflow_cnt - underflows);
			underflows = dev->underflow_cnt;
		}

		/* call application callback to get samples */
		if (dev->cb)
			dev->cb(&data_info);

		xfer = fl2k_get_next_xfer(dev, BUF_EMPTY);

		if (!xfer) {
			pthread_cond_wait(&dev->buf_cond, &dev->buf_mutex);
			/* in the meantime, the device might be gone */
			if (FL2K_RUNNING != dev->async_status)
				break;

			xfer = fl2k_get_next_xfer(dev, BUF_EMPTY);
			if (!xfer) {
				fprintf(stderr, "no free transfer, skipping"
						" input buffer\n");
				continue;
			}
		}

		/* We have an empty USB transfer buffer */
		xfer_info = (fl2k_xfer_info_t *)xfer->user_data;
		out_buf = (char *)xfer->buffer;

		/* Re-arrange and copy bytes in buffer for DACs */
		fl2k_convert_r(out_buf, data_info.r_buf, dev->xfer_buf_len,
			       data_info.sampletype_signed ? 128 : 0);

		fl2k_convert_g(out_buf, data_info.g_buf, dev->xfer_buf_len,
			       data_info.sampletype_signed ? 128 : 0);

		fl2k_convert_b(out_buf, data_info.b_buf, dev->xfer_buf_len,
			       data_info.sampletype_signed ? 128 : 0);

		xfer_info->seq = buf_cnt++;
		xfer_info->state = BUF_FILLED;
	}

	/* notify application if we've lost the device */
	if (dev->dev_lost && dev->cb) {
		data_info.device_error = 1;
		dev->cb(&data_info);
	}

	pthread_exit(NULL);
}


int fl2k_start_tx(fl2k_dev_t *dev, fl2k_tx_cb_t cb, void *ctx,
		  uint32_t buf_num)
{
	int r = 0;
	int i;
	pthread_attr_t attr;

	if (!dev || !cb)
		return FL2K_ERROR_INVALID_PARAM;

	dev->async_status = FL2K_RUNNING;
	dev->async_cancel = 0;

	dev->cb = cb;
	dev->cb_ctx = ctx;

	if (buf_num > 0)
		dev->xfer_num = buf_num;
	else
		dev->xfer_num = DEFAULT_BUF_NUMBER;

	/* have two spare buffers that can be filled while the
	 * others are submitted */
	dev->xfer_buf_num = dev->xfer_num + 2;
	dev->xfer_buf_len = FL2K_XFER_LEN;

	r = fl2k_alloc_submit_transfers(dev);
	if (r < 0)
		goto cleanup;

	pthread_mutex_init(&dev->buf_mutex, NULL);
	pthread_cond_init(&dev->buf_cond, NULL);
	pthread_attr_init(&attr);

	r = pthread_create(&dev->usb_worker_thread, &attr,
			   fl2k_usb_worker, (void *)dev);
	if (r < 0) {
		fprintf(stderr, "Error spawning USB worker thread!\n");
		goto cleanup;
	}

	r = pthread_create(&dev->sample_worker_thread, &attr,
			   fl2k_sample_worker, (void *)dev);
	if (r < 0) {
		fprintf(stderr, "Error spawning sample worker thread!\n");
		goto cleanup;
	}

	pthread_attr_destroy(&attr);

	return 0;

cleanup:
	_fl2k_free_async_buffers(dev);
	return FL2K_ERROR_BUSY;

}

int fl2k_stop_tx(fl2k_dev_t *dev)
{
	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	/* if streaming, try to cancel gracefully */
	if (FL2K_RUNNING == dev->async_status) {
		dev->async_status = FL2K_CANCELING;
		dev->async_cancel = 1;
		return 0;
	/* if called while in pending state, change the state forcefully */
	} else if (FL2K_INACTIVE != dev->async_status) {
		dev->async_status = FL2K_INACTIVE;
		return 0;
	}

	return FL2K_ERROR_BUSY;
}

int fl2k_i2c_read(fl2k_dev_t *dev, uint8_t i2c_addr, uint8_t reg_addr, uint8_t *data)
{
	int i, r, timeout = 1;
	uint32_t reg;

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	r = fl2k_read_reg(dev, 0x8020, &reg);
	if (r < 0)
		return r;

	/* apply mask, clearing bit 30 disables periodic repetition of read */
	reg &= 0x3ffc0000;

	/* set I2C register and address, select I2C read (bit 7) */
	reg |= (1 << 28) | (reg_addr << 8) | (1 << 7) | (i2c_addr & 0x7f);

	r = fl2k_write_reg(dev, 0x8020, reg);
	if (r < 0)
		return r;

	for (i = 0; i < 10; i++) {
		sleep_ms(10);

		r = fl2k_read_reg(dev, 0x8020, &reg);
		if (r < 0)
			return r;

		/* check if operation completed */ 
		if (reg & (1 << 31)) {
			timeout = 0;
			break;
		}
	}

	if (timeout)
		return FL2K_ERROR_TIMEOUT;

	/* check if slave responded and all data was read */
	if (reg & (0x0f << 24))
		return FL2K_ERROR_NOT_FOUND;

	/* read data from register 0x8024 */
	return libusb_control_transfer(dev->devh, CTRL_IN, 0x40,
				       0, 0x8024, data, 4, CTRL_TIMEOUT);
}

int fl2k_i2c_write(fl2k_dev_t *dev, uint8_t i2c_addr, uint8_t reg_addr, uint8_t *data)
{
	int i, r, timeout = 1;
	uint32_t reg;

	if (!dev)
		return FL2K_ERROR_INVALID_PARAM;

	/* write data to register 0x8028 */
	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0x41,
				    0, 0x8028, data, 4, CTRL_TIMEOUT);

	if (r < 0)
		return r;

	r = fl2k_read_reg(dev, 0x8020, &reg);
	if (r < 0)
		return r;

	/* apply mask, clearing bit 30 disables periodic repetition of read */
	reg &= 0x3ffc0000;

	/* set I2C register and address */
	reg |= (1 << 28) | (reg_addr << 8) | (i2c_addr & 0x7f);

	r = fl2k_write_reg(dev, 0x8020, reg);
	if (r < 0)
		return r;

	for (i = 0; i < 10; i++) {
		sleep_ms(10);

		r = fl2k_read_reg(dev, 0x8020, &reg);
		if (r < 0)
			return r;

		/* check if operation completed */ 
		if (reg & (1 << 31)) {
			timeout = 0;
			break;
		}
	}

	if (timeout)
		return FL2K_ERROR_TIMEOUT;

	/* check if slave responded and all data was written */
	if (reg & (0x0f << 24))
		return FL2K_ERROR_NOT_FOUND;

	return FL2K_SUCCESS;
}
