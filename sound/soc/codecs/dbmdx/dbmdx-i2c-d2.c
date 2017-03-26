/*
 * DSPG DBMD2 I2C interface driver
 *
 * Copyright (C) 2014 DSP Group
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
/* #define DEBUG */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/firmware.h>

#include "dbmdx-interface.h"
#include "dbmdx-va-regmap.h"
#include "dbmdx-vqe-regmap.h"
#include "dbmdx-i2c.h"
#include "dbmdx-i2c-sbl-d2.h"


static const u8 clr_crc[] = {0x5A, 0x03, 0x52, 0x0a, 0x00,
			     0x00, 0x00, 0x00, 0x00, 0x00};



static int dbmd2_i2c_boot(const struct firmware *fw, struct dbmdx_private *p,
		    const void *checksum, size_t chksum_len, int load_fw)
{
	int retry = RETRY_COUNT;
	int ret = 0;
	ssize_t send_bytes;
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	u8 rx_checksum[6];

	dev_dbg(i2c_p->dev, "%s\n", __func__);

	/* change to boot I2C address */
	i2c_p->client->addr = (unsigned short)(i2c_p->pdata->boot_addr);

	while (retry--) {

		if (p->active_fw == DBMDX_FW_PRE_BOOT) {

			/* reset DBMD2 chip */
			p->reset_sequence(p);

			/* delay before sending commands */
			if (p->clk_get_rate(p, DBMDX_CLK_MASTER) <= 32768)
				msleep(DBMDX_MSLEEP_I2C_D2_AFTER_RESET_32K);
			else
				usleep_range(DBMDX_USLEEP_I2C_D2_AFTER_RESET,
					DBMDX_USLEEP_I2C_D2_AFTER_RESET + 5000);
			if (p->set_i2c_freq_callback) {
				dev_dbg(p->dev,
					"%s: setting master bus to slow freq\n",
					__func__);
				p->set_i2c_freq_callback(
					i2c_p->client->adapter, I2C_FREQ_SLOW);
			}
			/* send SBL */
			send_bytes = write_i2c_data(p, sbl, sizeof(sbl));
			if (send_bytes != sizeof(sbl)) {
				dev_err(p->dev,
					"%s: -----------> load SBL error\n",
					 __func__);
				continue;
			}

			usleep_range(DBMDX_USLEEP_I2C_D2_AFTER_SBL,
				DBMDX_USLEEP_I2C_D2_AFTER_SBL + 1000);

			if (p->set_i2c_freq_callback) {
				msleep(DBMDX_MSLEEP_I2C_D2_CALLBACK);
				dev_dbg(p->dev,
					"%s: setting master bus to fast freq\n",
					__func__);
				p->set_i2c_freq_callback(
					i2c_p->client->adapter, I2C_FREQ_FAST);
			}

			/* send CRC clear command */
			ret = write_i2c_data(p, clr_crc, sizeof(clr_crc));
			if (ret != sizeof(clr_crc)) {
				dev_err(p->dev, "%s: failed to clear CRC\n",
					 __func__);
				continue;
			}
		} else {
			/* delay before sending commands */
			if (p->active_fw == DBMDX_FW_VQE)
				ret = send_i2c_cmd_vqe(p,
					DBMDX_VQE_SET_SWITCH_TO_BOOT_CMD,
					NULL);
			else if (p->active_fw == DBMDX_FW_VA)
				ret = send_i2c_cmd_va(p,
					DBMDX_VA_SWITCH_TO_BOOT,
					NULL);
			if (ret < 0) {
				dev_err(p->dev,
					"%s: failed to send 'Switch to BOOT' cmd\n",
					 __func__);
				continue;
			}
		}

		if (!load_fw)
			break;
		/* Sleep is needed here to ensure that chip is ready */
		msleep(DBMDX_MSLEEP_I2C_D2_AFTER_SBL);

		/* send firmware */
		send_bytes = write_i2c_data(p, fw->data, fw->size - 4);
		if (send_bytes != fw->size - 4) {
			dev_err(p->dev,
				"%s: -----------> load firmware error\n",
				__func__);
			continue;
		}

		msleep(DBMDX_MSLEEP_I2C_D2_BEFORE_FW_CHECKSUM);

		/* verify checksum */
		if (checksum) {
			ret = send_i2c_cmd_boot(i2c_p, DBMDX_READ_CHECKSUM);
			if (ret < 0) {
				dev_err(i2c_p->dev,
					"%s: could not read checksum\n",
					__func__);
				continue;
			}

			ret = i2c_master_recv(i2c_p->client, rx_checksum, 6);
			if (ret < 0) {
				dev_err(i2c_p->dev,
					"%s: could not read checksum data\n",
					__func__);
				continue;
			}

			ret = p->verify_checksum(p, checksum, &rx_checksum[2],
						 4);
			if (ret) {
				dev_err(p->dev, "%s: checksum mismatch\n",
					__func__);
				continue;
			}
		}

		dev_info(p->dev, "%s: ---------> firmware loaded\n",
			__func__);
		break;
	}

	/* no retries left, failed to boot */
	if (retry < 0) {
		dev_err(p->dev, "%s: failed to load firmware\n", __func__);
		return -1;
	}

	/* send boot command */
	ret = send_i2c_cmd_boot(i2c_p, DBMDX_FIRMWARE_BOOT);
	if (ret < 0) {
		dev_err(p->dev, "%s: booting the firmware failed\n", __func__);
		return -1;
	}

	/* wait some time */
	usleep_range(DBMDX_USLEEP_I2C_D2_AFTER_BOOT,
		DBMDX_USLEEP_I2C_D2_AFTER_BOOT + 1000);

	return 0;
}

static int dbmd2_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int rc;
	struct dbmdx_i2c_private *p;
	struct chip_interface *ci;

	rc = i2c_common_probe(client, id);

	if (rc < 0)
		return rc;

	ci = i2c_get_clientdata(client);
	p = (struct dbmdx_i2c_private *)ci->pdata;

	/* fill in chip interface functions */
	p->chip.boot = dbmd2_i2c_boot;

	return rc;
}


static const struct of_device_id dbmd2_i2c_of_match[] = {
	{ .compatible = "dspg,dbmd2-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, dbmd2_i2c_of_match);

static const struct i2c_device_id dbmd2_i2c_id[] = {
	{ "dbmdx-i2c", 0 },
	{ "dbmd2-i2c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, dbmd2_i2c_id);

static struct i2c_driver dbmd2_i2c_driver = {
	.driver = {
		.name = "dbmd2-i2c",
		.owner = THIS_MODULE,
		.of_match_table = dbmd2_i2c_of_match,
	},
	.probe =    dbmd2_i2c_probe,
	.remove =   i2c_common_remove,
	.id_table = dbmd2_i2c_id,
};

static int __init dbmd2_modinit(void)
{
	return i2c_add_driver(&dbmd2_i2c_driver);
}
module_init(dbmd2_modinit);

static void __exit dbmd2_exit(void)
{
	i2c_del_driver(&dbmd2_i2c_driver);
}
module_exit(dbmd2_exit);

MODULE_DESCRIPTION("DSPG DBMD2 I2C interface driver");
MODULE_LICENSE("GPL");
