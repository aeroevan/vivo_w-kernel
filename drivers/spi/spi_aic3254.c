/* linux/driver/spi/spi_aic3254.c
 *
 *
 * Copyright (C) 2009 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_aic3254.h>
#include <linux/spi/spi_aic3254_reg.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/wakelock.h>

static struct spi_device *codec_dev;
static struct mutex lock;
static int aic3254_opend;
static struct _CODEC_SPI_CMD **aic3254_uplink;
static struct _CODEC_SPI_CMD **aic3254_downlink;
static struct _CODEC_SPI_CMD **aic3254_minidsp;
static int suspend_flag;
static int aic3254_rx_mode;
static int aic3254_tx_mode;
static struct aic3254_ctl_ops default_ctl_ops;
static struct aic3254_ctl_ops *ctl_ops = &default_ctl_ops;

struct ecodec_aic3254_state {
	int enabled;
	struct clk *rx_mclk;
	struct clk *rx_sclk;
	struct wake_lock idlelock;
};
static struct ecodec_aic3254_state codec_clk;

/* function prototype */
int route_tx_enable(int, int);
int route_rx_enable(int, int);

void aic3254_register_ctl_ops(struct aic3254_ctl_ops *ops)
{
	ctl_ops = ops;
}

static int codec_spi_write(unsigned char addr, unsigned char data)
{
	unsigned char buffer[2];
	int rc;

	if (!codec_dev)
		return 0;

	codec_dev->bits_per_word = 16;
	buffer[0] = addr << 1;
	buffer[1] = data;
	rc = spi_write(codec_dev, buffer, 2);
	(void)rc;

	return 1;
}

static int codec_spi_read(unsigned char addr, unsigned char *data)
{
	int rc;
	u8 buffer[2] = {0, 0};
	u8 result[2] = {0, 0};

	codec_dev->bits_per_word = 16;
	buffer[0] = addr << 1 | 1;
	rc = spi_write_and_read(codec_dev, buffer, result, 2);
	if (rc < 0)
		return rc;

	*data = result[1];
	return 0;
}

static int aic3254_config(CODEC_SPI_CMD *cmds, int size)
{
	int i, retry, ret;
	unsigned char data;

	if (!codec_dev) {
		pr_err("%s: no spi device\n", __func__);
		return -EFAULT;
	}

	if (cmds == NULL) {
		pr_err("%s: invalid spi parameters\n", __func__);
		return -EINVAL;
	} else
		pr_info("%s: size = %d\n", __func__, size);

	/* when LCM power is off, spi transmission would fail sometime */
	if (suspend_flag && ctl_ops->panel_sleep_in) {
		ret = ctl_ops->panel_sleep_in();
		suspend_flag = 0;
		if (ret < 0)
			pr_err("%s: cannot make panel awake,"
				"it might failed on transmit SPI command\n"
				, __func__);
		else
			pr_info("%s: success on invoking panel_sleep_in\n"
				, __func__);
	}


	for (i = 0; i < size; i++) {
		switch (cmds[i].act) {
		case 'w':
			codec_spi_write(cmds[i].reg, cmds[i].data);
			break;
		case 'r':
			for (retry = AIC3254_MAX_RETRY; retry > 0; retry--) {
				ret = codec_spi_read(cmds[i].reg, &data);
				if (ret < 0)
					pr_err("%s: read fail %d, retry\n",
						__func__, ret);
				else if (data == cmds[i].data)
					break;
				hr_msleep(10);
			}
			if (retry <= 0)
				pr_err("%s: 3254 power down procedure"
					" ,flag 0x%02X=0x%02X(0x%02X)\n",
					__func__, cmds[i].reg,
					ret, cmds[i].data);
			break;
		case 'd':
			hr_msleep(cmds[i].data);
			break;
		default:
			break;
		}
	}

	return 0;
}

static int aic3254_config_ex(CODEC_SPI_CMD *cmds, int size)
{
	int i = 0;
	int ret = -EINVAL;
	struct spi_transfer *spi_t_cmds = NULL;
	struct spi_message m;
	unsigned char *buffer = NULL;
	unsigned char *ptr = NULL;

	if (!codec_dev) {
		pr_err("%s: no spi device\n", __func__);
		return -EFAULT;
	}

	if (cmds == NULL || size == 0) {
		pr_err("%s: invalid spi parameters\n", __func__);
		return -EINVAL;
	} else {
		/* pr_info("%s: size = %d", __func__, size); */
	}

	spi_t_cmds = (struct spi_transfer *) kmalloc(size*sizeof(struct spi_transfer), GFP_KERNEL);
	if (spi_t_cmds == NULL) {
		pr_err("%s: kmalloc spi transfer struct fail\n", __func__);
		goto error;
	} else
		memset(spi_t_cmds, 0, size*sizeof(struct spi_transfer));

	buffer = (unsigned char *) kmalloc(size * 2 * sizeof(unsigned char), GFP_KERNEL);
	if (buffer == NULL) {
		pr_err("%s: kmalloc buffer fail\n", __func__);
		goto error;
	} else
		memset(buffer, 0, size*sizeof(CODEC_SPI_CMD)*sizeof(unsigned char));

	spi_message_init(&m);
	for (i=0, ptr=buffer; i<size; i++, ptr+=2) {
		ptr[0] = cmds[i].reg << 1;
		ptr[1] = cmds[i].data;

		spi_t_cmds[i].tx_buf = ptr;
		spi_t_cmds[i].len = 2;
		spi_message_add_tail(&spi_t_cmds[i], &m);
	}
	codec_dev->bits_per_word = 16;
	ret = spi_sync(codec_dev, &m);

error:
	if (buffer)
		kfree(buffer);

	if (spi_t_cmds)
		kfree(spi_t_cmds);
	return ret;
}

static void aic3254_tx_config(int mode)
{
	/* use default setting when tx table doesn't be updated*/
	if (aic3254_uplink == NULL) {
		if (mode == UPLINK_OFF)
			route_tx_enable(mode, 0);
		else
			route_tx_enable(mode, 1);
		return;
	}

	if (mode != UPLINK_OFF && mode != POWER_OFF) {
		/* uplink_Wakeup */
		aic3254_config(
			&aic3254_uplink[UPLINK_WAKEUP][1],
			aic3254_uplink[UPLINK_WAKEUP][0].data);
	}

	/* route tx device */
	aic3254_config(&aic3254_uplink[mode][1],
			aic3254_uplink[mode][0].data);
}

static void aic3254_rx_config(int mode)
{
	/* use default setting when rx table doesn't be updated*/
	if (aic3254_downlink == NULL) {
		if (mode == DOWNLINK_OFF)
			route_rx_enable(mode, 0);
		else
			route_rx_enable(mode, 1);
		return;
	}

	if (mode != DOWNLINK_OFF && mode != POWER_OFF) {
		/* Downlink Wakeup */
		aic3254_config(
			&aic3254_downlink[DOWNLINK_WAKEUP][1],
			aic3254_downlink[DOWNLINK_WAKEUP][0].data);
	}

	/* route rx device */
	aic3254_config(&aic3254_downlink[mode][1],
				aic3254_downlink[mode][0].data);
}

static void aic3254_powerdown(void)
{
	struct ecodec_aic3254_state *drv = &codec_clk;
	if (aic3254_tx_mode != UPLINK_OFF || aic3254_rx_mode != DOWNLINK_OFF)
		return;

	pr_info("%s: power off AIC3254\n", __func__);
	wake_lock(&drv->idlelock);
	if (aic3254_uplink != NULL)
		aic3254_config(&aic3254_uplink[POWER_OFF][1],
				aic3254_uplink[POWER_OFF][0].data);
	else
		aic3254_config(CODEC_POWER_OFF, ARRAY_SIZE(CODEC_POWER_OFF));

	if (drv->enabled) {
		/* Disable MI2S RX master block */
		/* Disable MI2S RX bit clock */
		clk_disable(drv->rx_sclk);
		clk_disable(drv->rx_mclk);
		drv->enabled = 0;
		printk("%s: disable CLK\n", __func__);
	}
	wake_unlock(&drv->idlelock);
	return;
}
static void aic3254_loopback(int mode)
{
	if (!(ctl_ops->lb_dsp_init &&
		ctl_ops->lb_receiver_imic &&
		ctl_ops->lb_speaker_imic &&
		ctl_ops->lb_headset_emic)) {
		pr_info("%s: AIC3254 LOOPBACK not supported\n", __func__);
		return;
	}

	/* Init AIC3254 A00 */
	aic3254_config(ctl_ops->lb_dsp_init->data, ctl_ops->lb_dsp_init->len);

	pr_info("%s: set AIC3254 in LOOPBACK mode\n", __func__);
	switch (mode) {
	case 0:
		/* receiver v.s. imic */
		aic3254_config(ctl_ops->lb_receiver_imic->data,
				ctl_ops->lb_receiver_imic->len);
		break;
	case 1:
		/* speaker v.s. imic */
		aic3254_config(ctl_ops->lb_speaker_imic->data,
				ctl_ops->lb_speaker_imic->len);
		break;
	case 2:
		/* headphone v.s emic */
		aic3254_config(ctl_ops->lb_headset_emic->data,
				ctl_ops->lb_headset_emic->len);
	case 13:
		/* headphone v.s 2nd mic */
		if (ctl_ops->lb_headset_bmic)
			aic3254_config(ctl_ops->lb_headset_bmic->data,
				ctl_ops->lb_headset_bmic->len);
		else
			pr_info("%s: 2nd mic loopback not supported\n", __func__);
		break;
	}
}

int route_rx_enable(int path, int en)
{
	pr_info("%s: (%d,%d) uses 3254 default setting\n", __func__, path, en);
	if (en) {
		/* Downlink_Wakeup */
		aic3254_config(CODEC_DOWNLINK_ON,
				ARRAY_SIZE(CODEC_DOWNLINK_ON));
		/* Path switching */
		switch (path) {
		case FM_OUT_HEADSET:
			/* FM headset */
			aic3254_config(FM_In_Headphone,
					ARRAY_SIZE(FM_In_Headphone));
			aic3254_config(FM_Out_Headphone,
					ARRAY_SIZE(FM_Out_Headphone));
			break;
		case FM_OUT_SPEAKER:
			/* FM speaker */
			aic3254_config(FM_In_SPK,
					ARRAY_SIZE(FM_In_SPK));
			aic3254_config(FM_Out_SPK,
					ARRAY_SIZE(FM_Out_SPK));
			break;
		default:
			/* By pass */
			aic3254_config(Downlink_IMIC_Receiver,
					ARRAY_SIZE(Downlink_IMIC_Receiver));
			break;
		}
	} else {
		/* Downlink_Off */
		aic3254_config(CODEC_DOWNLINK_OFF,
				ARRAY_SIZE(CODEC_DOWNLINK_OFF));
	}

	return 0;
}

int route_tx_enable(int path, int en)
{
	pr_info("%s: (%d,%d) uses 3254 default setting\n", __func__, path, en);
	if (en) {
		/* Uplink_Wakeup */
		aic3254_config(CODEC_UPLINK_ON, ARRAY_SIZE(CODEC_UPLINK_ON));
		/* Path switching */
		switch (path) {
		case CALL_UPLINK_IMIC_RECEIVER:
		case CALL_UPLINK_IMIC_HEADSET:
		case CALL_UPLINK_IMIC_SPEAKER:
		case VOICERECORD_IMIC:
			/* By pass */
			aic3254_config(MECHA_Uplink_IMIC,
					ARRAY_SIZE(MECHA_Uplink_IMIC));
			break;
		case CALL_UPLINK_EMIC_HEADSET:
		case VOICERECORD_EMIC:
			aic3254_config(Uplink_EMIC,
					ARRAY_SIZE(Uplink_EMIC));
			break;
		}
	} else {
		/* Uplink_Off */
		aic3254_config(CODEC_UPLINK_OFF, ARRAY_SIZE(CODEC_UPLINK_OFF));
	}

	return 0;
}


void aic3254_set_mic_bias(int en) {

	if (en)
		aic3254_config(CODEC_MICBIAS_ON, ARRAY_SIZE(CODEC_MICBIAS_ON));
	else
		aic3254_config(CODEC_MICBIAS_OFF, ARRAY_SIZE(CODEC_MICBIAS_OFF));
}

static int aic3254_set_config(int config_tbl, int idx, int en)
{
	int len;
	struct ecodec_aic3254_state *drv = &codec_clk;
	pr_info("%s: table(0x%X) index(%d)\n", __func__, config_tbl, idx);

	wake_lock(&drv->idlelock);

	if (drv->enabled == 0) {
		/* enable MI2S RX master block */
		/* enable MI2S RX bit clock */
		clk_enable(drv->rx_mclk);
		clk_enable(drv->rx_sclk);
		printk("%s: enable CLK\n", __func__);
		drv->enabled = 1;
	}

	switch (config_tbl) {
	case AIC3254_CONFIG_TX:
		/* TX */
		pr_info("%s: enable tx\n", __func__);
		if (en) {
			if (ctl_ops->tx_amp_enable)
				ctl_ops->tx_amp_enable(0);

			aic3254_tx_config(idx);
			aic3254_tx_mode = idx;

			if (ctl_ops->tx_amp_enable)
				ctl_ops->tx_amp_enable(1);
		} else {
			aic3254_tx_config(UPLINK_OFF);
			aic3254_tx_mode = UPLINK_OFF;
		}
		break;
	case AIC3254_CONFIG_RX:
		/* RX */
		pr_info("%s: enable rx\n", __func__);
		if (en) {
			if (ctl_ops->rx_amp_enable)
				ctl_ops->rx_amp_enable(0);

			aic3254_rx_config(idx);
			aic3254_rx_mode = idx;

			if (ctl_ops->rx_amp_enable)
				ctl_ops->rx_amp_enable(1);
		} else {
			aic3254_rx_config(DOWNLINK_OFF);
			aic3254_rx_mode = DOWNLINK_OFF;
		}
		break;
	case AIC3254_CONFIG_MEDIA:
		if (aic3254_minidsp == NULL)
			return -EFAULT;
		len = (aic3254_minidsp[idx][0].reg << 8)
			| aic3254_minidsp[idx][0].data;

		pr_info("%s: miniDSP command len = %d\n", __func__, len);
		pr_info("%s: rx mode %d, tx mode %d\n",
			__func__, aic3254_rx_mode, aic3254_tx_mode);

		if (ctl_ops->rx_amp_enable)
			ctl_ops->rx_amp_enable(0);

		/* step 1: power off first */
		if (aic3254_rx_mode != DOWNLINK_OFF)
			aic3254_rx_config(DOWNLINK_OFF);

		/* step 2: config DSP */
		aic3254_config(&aic3254_minidsp[idx][1], len);

		/* step 3: switch back to original path */
		if (aic3254_rx_mode != DOWNLINK_OFF)
			aic3254_rx_config(aic3254_rx_mode);
		if (aic3254_tx_mode != UPLINK_OFF)
			aic3254_tx_config(aic3254_tx_mode);

		if (ctl_ops->rx_amp_enable)
			ctl_ops->rx_amp_enable(1);

		pr_info("%s: configure minidsp done\n", __func__);
		break;
	}

	wake_unlock(&drv->idlelock);
	return 0;
}

static int aic3254_open(struct inode *inode, struct file *pfile)
{
	int ret = 0;

	mutex_lock(&lock);
	if (aic3254_opend) {
		pr_err("%s: busy\n", __func__);
		ret = -EBUSY;
	} else
		aic3254_opend = 1;
	mutex_unlock(&lock);

	return ret;
}

static int aic3254_release(struct inode *inode, struct file *pfile)
{
	mutex_lock(&lock);
	aic3254_opend = 0;
	mutex_unlock(&lock);

	return 0;
}

static int aic3254_ioctl(struct inode *inode, struct file *file,
		unsigned int cmd, unsigned long argc)
{
	struct AIC3254_PARAM para;
	void *table;
	int ret = 0, i = 0, mem_size, volume = 0;
	CODEC_SPI_CMD reg[2];
	unsigned char data;

	switch (cmd) {
	case AIC3254_SET_TX_PARAM:
	case AIC3254_SET_RX_PARAM:
		if (copy_from_user(&para, (void *)argc, sizeof(para))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}

		pr_info("%s: parameters(%d, %d, %p)\n", __func__,
				para.row_num, para.col_num, para.cmd_data);
		if (cmd == AIC3254_SET_TX_PARAM)
			table = aic3254_uplink[0];
		else
			table = aic3254_downlink[0];

		/* confirm indicated size doesn't exceed the allocated one */
		if (para.row_num > IO_CTL_ROW_MAX
				|| para.col_num != IO_CTL_COL_MAX) {
			pr_err("%s: data size mismatch with allocated"
					" memory (%d,%d)\n", __func__,
					IO_CTL_ROW_MAX, IO_CTL_COL_MAX);
			return -EFAULT;
		}

		mem_size = para.row_num * para.col_num * sizeof(CODEC_SPI_CMD);
		if (copy_from_user(table, para.cmd_data, mem_size)) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}

		/* invoking initialization procedure of AIC3254 */
		if (cmd == AIC3254_SET_TX_PARAM)
			aic3254_tx_config(INITIAL);

		pr_info("%s: update table(%d,%d) successfully\n",
				__func__, para.row_num, para.col_num);
			break;
       case AIC3254_SET_DSP_PARAM:
		if (copy_from_user(&para, (void *)argc, sizeof(para))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
			}
		pr_info("%s: parameters(%d, %d, %p)\n", __func__,
				para.row_num, para.col_num, para.cmd_data);

		table = aic3254_minidsp[0];

		/* confirm indicated size doesn't exceed the allocated one */
		if (para.row_num > MINIDSP_ROW_MAX
				|| para.col_num != MINIDSP_COL_MAX) {
			pr_err("%s: data size mismatch with allocated"
					" memory (%d,%d)\n", __func__,
					MINIDSP_ROW_MAX, MINIDSP_COL_MAX);
			return -EFAULT;
			}

		mem_size = para.row_num * para.col_num * sizeof(CODEC_SPI_CMD);
		if (copy_from_user(table, para.cmd_data, mem_size)) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}

		pr_info("%s: update table(%d,%d) successfully\n",
				__func__, para.row_num, para.col_num);
		break;
	case AIC3254_CONFIG_TX:
	case AIC3254_CONFIG_RX:
	case AIC3254_CONFIG_MEDIA:
		if (copy_from_user(&i, (void *)argc, sizeof(int))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}
		pr_info("%s: index %d for %X\n", __func__, i, cmd);
		ret = aic3254_set_config(cmd, i, 1);
		if (ret < 0)
			pr_err("%s: configure(%d) error %d\n",
				__func__, i, ret);
		break;
	case AIC3254_CONFIG_VOLUME_L:
		if (copy_from_user(&volume, (void *)argc, sizeof(int))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}

		if (volume < -127 || volume > 48) {
			pr_err("%s: volume out of range\n", __func__);
			return -EFAULT;
		}

		pr_info("%s: AIC3254 config left volume %d\n",
				__func__, volume);

		CODEC_SET_VOLUME_L[1].data = volume;
		aic3254_config_ex(CODEC_SET_VOLUME_L, ARRAY_SIZE(CODEC_SET_VOLUME_L));
		break;
	case AIC3254_CONFIG_VOLUME_R:
		if (copy_from_user(&volume, (void *)argc, sizeof(int))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}

		if (volume < -127 || volume > 48) {
			pr_err("%s: volume out of range\n", __func__);
			return -EFAULT;
		}

		pr_info("%s: AIC3254 config right volume %d\n",
				__func__, volume);

		CODEC_SET_VOLUME_R[1].data = volume;
		aic3254_config_ex(CODEC_SET_VOLUME_R, ARRAY_SIZE(CODEC_SET_VOLUME_R));
		break;
	case AIC3254_DUMP_PAGES:
		if (copy_from_user(&i, (void *)argc, sizeof(int))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}
		if (i > AIC3254_MAX_PAGES) {
			pr_err("%s: invalid page number %d\n", __func__, i);
			return -EINVAL;
		}

		pr_info("========== %s: dump page %d ==========\n",
				__func__, i);
		/* indicated page number to AIC3254 */
		codec_spi_write(0x00, i);
		for (i = 0; i < AIC3254_MAX_REGS; i++) {
			ret = codec_spi_read(i, &data);
			if (ret < 0)
				pr_err("read fail on register 0x%X\n", i);
			else
				pr_info("(0x%02X, 0x%02X)\n", i, data);
		}
		pr_info("=============================================\n");
		break;
	case AIC3254_WRITE_REG:
		if (copy_from_user(&reg, (void *)argc,
					sizeof(CODEC_SPI_CMD)*2)) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}
		pr_info("%s: command list (%c,%02X,%02X) (%c,%02X,%02X)\n",
				__func__, reg[0].act, reg[0].reg, reg[0].data,
				reg[1].act, reg[1].reg, reg[1].data);
		aic3254_config_ex(reg, 2);
		break;
	case AIC3254_READ_REG:
		if (copy_from_user(&reg, (void *)argc,
					sizeof(CODEC_SPI_CMD)*2)) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}
		for (i = 0; i < 2; i++) {
			if (reg[i].act == 'r' || reg[i].act == 'R')
				codec_spi_read(reg[i].reg, &reg[i].data);
			else if (reg[i].act == 'w' || reg[i].act == 'W')
				codec_spi_write(reg[i].reg, reg[i].data);
			else
				return -EINVAL;
		}
		if (copy_to_user((void *)argc, &reg, sizeof(CODEC_SPI_CMD)*2)) {
			pr_err("%s: failed on copy_to_user\n", __func__);
			return -EFAULT;
		}
		break;
	case AIC3254_POWERDOWN:
		aic3254_powerdown();
		break;
	case AIC3254_LOOPBACK:
		if (copy_from_user(&i, (void *)argc, sizeof(int))) {
			pr_err("%s: failed on copy_from_user\n", __func__);
			return -EFAULT;
		}
		pr_info("%s: index %d for LOOPBACK\n", __func__, i);
		aic3254_loopback(i);
		break;
	default:
		pr_err("%s: invalid command %d\n", __func__, _IOC_NR(cmd));
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations aic3254_fops = {
	.owner = THIS_MODULE,
	.open = aic3254_open,
	.release = aic3254_release,
	.ioctl = aic3254_ioctl,
};

static struct miscdevice aic3254_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "codec_aic3254",
	.fops = &aic3254_fops,
};

static  CODEC_SPI_CMD** init_2d_array(int row_sz, int col_sz)
{
	CODEC_SPI_CMD *table = NULL;
	CODEC_SPI_CMD **table_ptr = NULL;
	int i = 0;

	table_ptr = kzalloc(row_sz * sizeof(CODEC_SPI_CMD *), GFP_KERNEL);
	table = kzalloc(row_sz * col_sz * sizeof(CODEC_SPI_CMD), GFP_KERNEL);
	if (table_ptr == NULL || table == NULL) {
		pr_err("%s: out of memory\n", __func__);
		kfree(table);
		kfree(table_ptr);
	} else
		for (i = 0; i < row_sz; i++)
			table_ptr[i] = (CODEC_SPI_CMD *)table + i * col_sz;

	return table_ptr;
}

static int spi_aic3254_probe(struct spi_device *aic3254)
{
	pr_info("%s\n", __func__);

	codec_dev = aic3254;

	/* Boot up */
#if 0
	aic3254_config(CODEC_INIT_REG, ARRAY_SIZE(CODEC_INIT_REG));
	aic3254_config(CODEC_DOWNLINK_OFF, ARRAY_SIZE(CODEC_DOWNLINK_OFF));
	aic3254_config(CODEC_UPLINK_OFF, ARRAY_SIZE(CODEC_UPLINK_OFF));
	aic3254_config(CODEC_POWER_OFF, ARRAY_SIZE(CODEC_POWER_OFF));
#endif

	aic3254_tx_mode = UPLINK_OFF;
	aic3254_rx_mode = DOWNLINK_OFF;

	/* request space for firmware data of AIC3254 */
	aic3254_uplink = init_2d_array(IO_CTL_ROW_MAX, IO_CTL_COL_MAX);
	aic3254_downlink = init_2d_array(IO_CTL_ROW_MAX, IO_CTL_COL_MAX);
	aic3254_minidsp = init_2d_array(MINIDSP_ROW_MAX, MINIDSP_COL_MAX);
	return 0;
}


static int spi_aic3254_suspend(struct spi_device *aic3254, pm_message_t pmsg)
{
	pr_info("%s\n", __func__);
	suspend_flag = 1;
	return 0;
}

static int spi_aic3254_resume(struct spi_device *aic3254)
{
	pr_info("%s\n", __func__);
	return 0;
}

static int spi_aic3254_remove(struct spi_device *aic3254)
{
	pr_info("%s\n", __func__);

	/* release allocated memory in this driver */
	if (aic3254_uplink != NULL) {
		kfree(aic3254_uplink[0]);
		kfree(aic3254_uplink);
		aic3254_uplink = NULL;
	}
	if (aic3254_downlink != NULL) {
		kfree(aic3254_downlink[0]);
		kfree(aic3254_downlink);
		aic3254_downlink = NULL;
	}
	if (aic3254_minidsp != NULL) {
		kfree(aic3254_minidsp[0]);
		kfree(aic3254_minidsp);
		aic3254_minidsp = NULL;
	}

	return 0;
}

static struct spi_driver spi_aic3254 = {
	.driver = {
		.name = "spi_aic3254",
		.owner = THIS_MODULE,
	},
	.probe = spi_aic3254_probe,
	.suspend = spi_aic3254_suspend,
	.resume = spi_aic3254_resume,
	.remove = spi_aic3254_remove,
};

static int __init spi_aic3254_init(void)
{
	int ret = 0;
	struct ecodec_aic3254_state *codec_drv =  &codec_clk;

	pr_info("%s\n", __func__);
	mutex_init(&lock);

	ret = spi_register_driver(&spi_aic3254);
	if (ret < 0) {
		pr_err("%s:failed to register spi driver(%d)\n", __func__, ret);
		return ret;
	}

	ret = misc_register(&aic3254_misc);
	if (ret < 0) {
		pr_err("%s:failed to register misc device\n", __func__);
		spi_unregister_driver(&spi_aic3254);
		return ret;
	}

	codec_drv->rx_mclk = clk_get(NULL, "mi2s_codec_rx_m_clk");
	if (IS_ERR(codec_drv->rx_mclk)) {
		pr_err("%s:failed to get mi2s mclk\n", __func__);
		misc_deregister(&aic3254_misc);
		spi_unregister_driver(&spi_aic3254);
		return -ENODEV;
	}
	codec_drv->rx_sclk = clk_get(NULL, "mi2s_codec_rx_s_clk");
	if (IS_ERR(codec_drv->rx_sclk)) {
		pr_err("%s:failed to get mi2s sclk\n", __func__);
		misc_deregister(&aic3254_misc);
		spi_unregister_driver(&spi_aic3254);
		clk_put(codec_drv->rx_mclk);
		return -ENODEV;
	}

	wake_lock_init(&codec_drv->idlelock, WAKE_LOCK_IDLE,
			"aic3254_lock");

	return 0;
}
module_init(spi_aic3254_init);

static void __exit spi_aic3254_exit(void)
{
	struct ecodec_aic3254_state *codec_drv =  &codec_clk;

	spi_unregister_driver(&spi_aic3254);
	misc_deregister(&aic3254_misc);

	clk_put(codec_drv->rx_mclk);
	clk_put(codec_drv->rx_sclk);
	return;
}
module_exit(spi_aic3254_exit);

MODULE_LICENSE("GPL");
