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

#ifndef __FL2K_H
#define __FL2K_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <osmo-fl2k_export.h>

enum fl2k_error {
	FL2K_SUCCESS = 0,
	FL2K_TRUE = 1,
	FL2K_ERROR_INVALID_PARAM = -1,
	FL2K_ERROR_NO_DEVICE = -2,
	FL2K_ERROR_NOT_FOUND = -5,
	FL2K_ERROR_BUSY = -6,
	FL2K_ERROR_TIMEOUT = -7,
	FL2K_ERROR_NO_MEM = -11,
};

typedef struct fl2k_data_info {
	/* information provided by library */
	void *ctx;
	uint32_t underflow_cnt;		/* underflows since last callback */
	uint32_t len;			/* buffer length */
	int using_zerocopy;		/* using zerocopy kernel buffers */
	int device_error;		/* device error happened, terminate application */

	/* filled in by application */
	int sampletype_signed;		/* are samples signed or unsigned? */
	char *r_buf;			/* pointer to red buffer */
	char *g_buf;			/* pointer to green buffer */
	char *b_buf;			/* pointer to blue buffer */
} fl2k_data_info_t;

typedef struct fl2k_dev fl2k_dev_t;

/** The transfer length was chosen by the following criteria:
 * - Must be a supported resolution of the FL2000DX
 * - Must be a multiple of 61440 bytes (URB payload length),
 *   which is important for using the DAC without HSYNC/VSYNC blanking,
 *   otherwise a couple of samples are missing between every buffer
 * - Should be smaller than 4MB in order to be allocatable by kmalloc()
 *   for zerocopy transfers
 **/
#define FL2K_BUF_LEN		(1280 * 1024)
#define FL2K_XFER_LEN		(FL2K_BUF_LEN * 3)

FL2K_API uint32_t fl2k_get_device_count(void);

FL2K_API const char* fl2k_get_device_name(uint32_t index);

FL2K_API int fl2k_open(fl2k_dev_t **dev, uint32_t index);

FL2K_API int fl2k_close(fl2k_dev_t *dev);

/* configuration functions */

/*!
 * Set the sample rate (pixel clock) for the device
 *
 * \param dev the device handle given by fl2k_open()
 * \param samp_rate the sample rate to be set, maximum value depends
 * 	  on host and USB controller
 * \return 0 on success, -EINVAL on invalid rate
 */
FL2K_API int fl2k_set_sample_rate(fl2k_dev_t *dev, uint32_t target_freq);

/*!
 * Get actual sample rate the device is configured to.
 *
 * \param dev the device handle given by fl2k_open()
 * \return 0 on error, sample rate in Hz otherwise
 */
FL2K_API uint32_t fl2k_get_sample_rate(fl2k_dev_t *dev);

/* streaming functions */

typedef void(*fl2k_tx_cb_t)(fl2k_data_info_t *data_info);

/*!
 * Starts the tx thread. This function will block until
 * it is being canceled using fl2k_stop_tx()
 *
 * \param dev the device handle given by fl2k_open()
 * \param ctx user specific context to pass via the callback function
 * \param buf_num optional buffer count, buf_num * FL2K_BUF_LEN = overall buffer size
 *		  set to 0 for default buffer count (4)
 * \return 0 on success
 */
FL2K_API int fl2k_start_tx(fl2k_dev_t *dev, fl2k_tx_cb_t cb,
		     void *ctx, uint32_t buf_num);

/*!
 * Cancel all pending asynchronous operations on the device.
 *
 * \param dev the device handle given by fl2k_open()
 * \return 0 on success
 */
FL2K_API int fl2k_stop_tx(fl2k_dev_t *dev);

/*!
 * Read 4 bytes via the FL2K I2C bus
 *
 * \param dev the device handle given by fl2k_open()
 * \param i2c_addr address of the I2C device
 * \param reg_addr start address of the 4 bytes to be read
 * \param data pointer to byte array of size 4
 * \return 0 on success
 * \note A read operation will look like this on the bus:
 *       START, I2C_ADDR(W), REG_ADDR,   REP_START, I2C_ADDR(R), DATA[0], STOP
 *       START, I2C_ADDR(W), REG_ADDR+1, REP_START, I2C_ADDR(R), DATA[1], STOP
 *       START, I2C_ADDR(W), REG_ADDR+2, REP_START, I2C_ADDR(R), DATA[2], STOP
 *       START, I2C_ADDR(W), REG_ADDR+3, REP_START, I2C_ADDR(R), DATA[3], STOP
 */
FL2K_API int fl2k_i2c_read(fl2k_dev_t *dev, uint8_t i2c_addr,
		           uint8_t reg_addr, uint8_t *data);

/*!
 * Write 4 bytes via the FL2K I2C bus
 *
 * \param dev the device handle given by fl2k_open()
 * \param i2c_addr address of the I2C device
 * \param reg_addr start address of the 4 bytes to be written
 * \param data pointer to byte array of size 4
 * \return 0 on success
 * \note A write operation will look like this on the bus:
 *       START, I2C_ADDR(W), REG_ADDR,   DATA[0], STOP
 *       START, I2C_ADDR(W), REG_ADDR+1, DATA[1], STOP
 *       START, I2C_ADDR(W), REG_ADDR+2, DATA[2], STOP
 *       START, I2C_ADDR(W), REG_ADDR+3, DATA[3], STOP
 */
FL2K_API int fl2k_i2c_write(fl2k_dev_t *dev, uint8_t i2c_addr,
			    uint8_t reg_addr, uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif /* __FL2K_H */
