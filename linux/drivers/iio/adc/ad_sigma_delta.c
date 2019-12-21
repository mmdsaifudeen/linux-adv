/*
 * Support code for Analog Devices Sigma-Delta ADCs
 *
 * Copyright 2012 Analog Devices Inc.
 *  Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 * Licensed under the GPL-2.
 */

#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/err.h>
#include <linux/module.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/adc/ad_sigma_delta.h>

#include <linux/spi/spi-engine.h>

#include <asm/unaligned.h>

#define AD_SD_COMM_CHAN_MASK	0x3

#define AD_SD_REG_COMM		0x00
#define AD_SD_REG_DATA		0x03

/**
 * ad_sd_set_comm() - Set communications register
 *
 * @sigma_delta: The sigma delta device
 * @comm: New value for the communications register
 */
void ad_sd_set_comm(struct ad_sigma_delta *sigma_delta, uint8_t comm)
{
	/* Some variants use the lower two bits of the communications register
	 * to select the channel */
	sigma_delta->comm = comm & AD_SD_COMM_CHAN_MASK;
}
EXPORT_SYMBOL_GPL(ad_sd_set_comm);

/**
 * ad_sd_write_reg() - Write a register
 *
 * @sigma_delta: The sigma delta device
 * @reg: Address of the register
 * @size: Size of the register (0-3)
 * @val: Value to write to the register
 *
 * Returns 0 on success, an error code otherwise.
 **/
int ad_sd_write_reg(struct ad_sigma_delta *sigma_delta, unsigned int reg,
	unsigned int size, unsigned int val)
{
	uint8_t *data = sigma_delta->data;
	struct spi_transfer t = {
		.tx_buf		= data,
		.len		= size + 1,
		.cs_change	= sigma_delta->keep_cs_asserted,
	};
	struct spi_message m;
	int ret;

	data[0] = (reg << sigma_delta->info->addr_shift) | sigma_delta->comm;

	switch (size) {
	case 3:
		data[1] = val >> 16;
		data[2] = val >> 8;
		data[3] = val;
		break;
	case 2:
		put_unaligned_be16(val, &data[1]);
		break;
	case 1:
		data[1] = val;
		break;
	case 0:
		break;
	default:
		return -EINVAL;
	}

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	if (sigma_delta->bus_locked)
		ret = spi_sync_locked(sigma_delta->spi, &m);
	else
		ret = spi_sync(sigma_delta->spi, &m);

	return ret;
}
EXPORT_SYMBOL_GPL(ad_sd_write_reg);

static void ad_sd_prepare_read_reg(struct ad_sigma_delta *sigma_delta,
	struct spi_message *m, struct spi_transfer *t, unsigned int reg,
	unsigned int size, uint8_t *tx_buf, uint8_t *rx_buf, bool cs_change)
{
	memset(t, 0, sizeof(*t) * 2);
	t[1].rx_buf = rx_buf;
	t[1].len = size;
	t[1].cs_change = cs_change;

	spi_message_init(m);

	if (sigma_delta->info->has_registers) {
		tx_buf[0] = reg << sigma_delta->info->addr_shift;
		tx_buf[0] |= sigma_delta->info->read_mask;
		tx_buf[0] |= sigma_delta->comm;
		t[0].tx_buf = tx_buf,
		t[0].len = 1,
		spi_message_add_tail(&t[0], m);
	}
	spi_message_add_tail(&t[1], m);
}

static int ad_sd_read_reg_raw(struct ad_sigma_delta *sigma_delta,
	unsigned int reg, unsigned int size, uint8_t *val)
{
	struct spi_message m;
	struct spi_transfer t[2];
	int ret;

	ad_sd_prepare_read_reg(sigma_delta, &m, t, reg, size,
		sigma_delta->data, val, sigma_delta->keep_cs_asserted);

	if (sigma_delta->bus_locked)
		ret = spi_sync_locked(sigma_delta->spi, &m);
	else
		ret = spi_sync(sigma_delta->spi, &m);

	return ret;
}

/**
 * ad_sd_read_reg() - Read a register
 *
 * @sigma_delta: The sigma delta device
 * @reg: Address of the register
 * @size: Size of the register (1-4)
 * @val: Read value
 *
 * Returns 0 on success, an error code otherwise.
 **/
int ad_sd_read_reg(struct ad_sigma_delta *sigma_delta,
	unsigned int reg, unsigned int size, unsigned int *val)
{
	int ret;

	ret = ad_sd_read_reg_raw(sigma_delta, reg, size, sigma_delta->data);
	if (ret < 0)
		goto out;

	switch (size) {
	case 4:
		*val = get_unaligned_be32(sigma_delta->data);
		break;
	case 3:
		*val = (sigma_delta->data[0] << 16) |
			(sigma_delta->data[1] << 8) |
			sigma_delta->data[2];
		break;
	case 2:
		*val = get_unaligned_be16(sigma_delta->data);
		break;
	case 1:
		*val = sigma_delta->data[0];
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	return ret;
}
EXPORT_SYMBOL_GPL(ad_sd_read_reg);

/**
 * ad_sd_reset() - Reset the serial interface
 *
 * @sigma_delta: The sigma delta device
 * @reset_length: Number of SCLKs with DIN = 1
 *
 * Returns 0 on success, an error code otherwise.
 **/
int ad_sd_reset(struct ad_sigma_delta *sigma_delta,
	unsigned int reset_length)
{
	uint8_t *buf;
	unsigned int size;
	int ret;

	size = DIV_ROUND_UP(reset_length, 8);
	buf = kcalloc(size, sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0xff, size);
	ret = spi_write(sigma_delta->spi, buf, size);
	kfree(buf);

	return ret;
}
EXPORT_SYMBOL_GPL(ad_sd_reset);

static int ad_sd_calibrate(struct ad_sigma_delta *sigma_delta,
	unsigned int mode, unsigned int channel)
{
	int ret;

	ret = ad_sigma_delta_set_channel(sigma_delta, 0, channel);
	if (ret)
		return ret;

	spi_bus_lock(sigma_delta->spi->master);
	sigma_delta->bus_locked = true;
	sigma_delta->keep_cs_asserted = true;
	reinit_completion(&sigma_delta->completion);

	ret = ad_sigma_delta_set_mode(sigma_delta, mode);
	if (ret < 0)
		goto out;

	sigma_delta->irq_dis = false;
	enable_irq(sigma_delta->spi->irq);
	ret = wait_for_completion_timeout(&sigma_delta->completion, 2*HZ);
	if (ret == 0) {
		sigma_delta->irq_dis = true;
		disable_irq_nosync(sigma_delta->spi->irq);
		ret = -EIO;
	} else {
		ret = 0;
	}
out:
	sigma_delta->keep_cs_asserted = false;
	ad_sigma_delta_set_mode(sigma_delta, AD_SD_MODE_IDLE);
	sigma_delta->bus_locked = false;
	spi_bus_unlock(sigma_delta->spi->master);

	return ret;
}

/**
 * ad_sd_calibrate_all() - Performs channel calibration
 * @sigma_delta: The sigma delta device
 * @cb: Array of channels and calibration type to perform
 * @n: Number of items in cb
 *
 * Returns 0 on success, an error code otherwise.
 **/
int ad_sd_calibrate_all(struct ad_sigma_delta *sigma_delta,
	const struct ad_sd_calib_data *cb, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ad_sd_calibrate(sigma_delta, cb[i].mode, cb[i].channel);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ad_sd_calibrate_all);

static int ad_sigma_delta_set_active_slots(struct ad_sigma_delta *sigma_delta,
	unsigned int active_slots)
{
	unsigned int i;
	int ret;

	/* Disable unused slots */
	for (i = active_slots; i < sigma_delta->active_slots; i++) {
		ret = ad_sigma_delta_set_channel(sigma_delta, i,
			AD_SD_SLOT_DISABLE);
	}
	sigma_delta->active_slots = active_slots;

	return 0;
}

/**
 * ad_sigma_delta_single_conversion() - Performs a single data conversion
 * @indio_dev: The IIO device
 * @chan: The conversion is done for this channel
 * @val: Pointer to the location where to store the read value
 *
 * Returns: 0 on success, an error value otherwise.
 */
int ad_sigma_delta_single_conversion(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, int *val)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	unsigned int sample, raw_sample;
	unsigned int data_reg;
	int ret = 0;

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	mutex_lock(&indio_dev->mlock);
	ad_sigma_delta_prepare_channel(sigma_delta, 0, chan);
	ad_sigma_delta_set_channel(sigma_delta, 0, chan->address);

	ad_sigma_delta_set_active_slots(sigma_delta, 1);

	spi_bus_lock(sigma_delta->spi->master);
	sigma_delta->bus_locked = true;
	sigma_delta->keep_cs_asserted = true;
	reinit_completion(&sigma_delta->completion);

	ad_sigma_delta_set_mode(sigma_delta, AD_SD_MODE_SINGLE);

	sigma_delta->irq_dis = false;
	enable_irq(sigma_delta->spi->irq);
	ret = wait_for_completion_interruptible_timeout(
			&sigma_delta->completion, HZ);

	if (ret == 0)
		ret = -EIO;
	if (ret < 0)
		goto out;

	if (sigma_delta->info->data_reg != 0)
		data_reg = sigma_delta->info->data_reg;
	else
		data_reg = AD_SD_REG_DATA;

	ret = ad_sd_read_reg(sigma_delta, data_reg,
		DIV_ROUND_UP(chan->scan_type.realbits + chan->scan_type.shift, 8),
		&raw_sample);

out:
	if (!sigma_delta->irq_dis) {
		disable_irq_nosync(sigma_delta->spi->irq);
		sigma_delta->irq_dis = true;
	}

	sigma_delta->keep_cs_asserted = false;
	ad_sigma_delta_set_mode(sigma_delta, AD_SD_MODE_IDLE);
	sigma_delta->bus_locked = false;
	spi_bus_unlock(sigma_delta->spi->master);
	mutex_unlock(&indio_dev->mlock);

	if (ret)
		return ret;

	sample = raw_sample >> chan->scan_type.shift;
	sample &= (1 << chan->scan_type.realbits) - 1;
	*val = sample;

	ret = ad_sigma_delta_postprocess_sample(sigma_delta, raw_sample);
	if (ret)
		return ret;

	return IIO_VAL_INT;
}
EXPORT_SYMBOL_GPL(ad_sigma_delta_single_conversion);

static void ad_sd_prepare_transfer_msg(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	uint8_t *tx = sigma_delta->buf_data + indio_dev->scan_bytes;
	uint8_t *rx = sigma_delta->buf_data;
	unsigned int reg_size;
	unsigned int data_reg;

	reg_size = indio_dev->channels[0].scan_type.realbits +
			indio_dev->channels[0].scan_type.shift;
	reg_size = DIV_ROUND_UP(reg_size, 8);

	if (sigma_delta->info->data_reg != 0)
		data_reg = sigma_delta->info->data_reg;
	else
		data_reg = AD_SD_REG_DATA;

	/* We store 24 bit samples in a 32 bit word. Keep the upper
	 * byte set to zero. */
	if (reg_size == 3)
		rx += 1;
	ad_sd_prepare_read_reg(sigma_delta, &sigma_delta->spi_msg,
		sigma_delta->spi_transfer, data_reg, reg_size, tx,
		rx, true);
}

static int ad_sd_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	unsigned int i, slot;
	int ret;

	slot = 0;
	for_each_set_bit(i, indio_dev->active_scan_mask, indio_dev->masklength) {
		ret = ad_sigma_delta_prepare_channel(sigma_delta, slot,
			&indio_dev->channels[i]);
		if (ret)
			goto err_predisable;
		ret = ad_sigma_delta_set_channel(sigma_delta, slot,
			indio_dev->channels[i].address);
		if (ret)
			goto err_predisable;
		slot++;
	}

	kfree(sigma_delta->buf_data);
	sigma_delta->buf_data = kzalloc(indio_dev->scan_bytes + 1, GFP_KERNEL);
	if (!sigma_delta->buf_data)
		return -ENOMEM;

	ad_sigma_delta_set_active_slots(sigma_delta, slot);
	sigma_delta->current_slot = 0;

	spi_bus_lock(sigma_delta->spi->master);
	sigma_delta->bus_locked = true;
	sigma_delta->keep_cs_asserted = true;

	ad_sd_prepare_transfer_msg(indio_dev);

	if (indio_dev->currentmode == INDIO_BUFFER_HARDWARE) {
		sigma_delta->spi_transfer[1].rx_buf = (void *)-1;
		spi_engine_offload_load_msg(sigma_delta->spi, &sigma_delta->spi_msg);
		spi_engine_offload_enable(sigma_delta->spi, true);
	} else {
		sigma_delta->spi_transfer[1].rx_buf = sigma_delta->buf_data;
		sigma_delta->irq_dis = false;
		enable_irq(sigma_delta->spi->irq);
	}

	ret = ad_sigma_delta_set_mode(sigma_delta, AD_SD_MODE_CONTINUOUS);
	if (ret)
		goto err_unlock;

	return 0;

err_unlock:
	spi_bus_unlock(sigma_delta->spi->master);
err_predisable:

	return ret;
}

static int ad_sd_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);

	if (indio_dev->currentmode == INDIO_BUFFER_HARDWARE) {
		spi_engine_offload_enable(sigma_delta->spi, false);
	} else {
		reinit_completion(&sigma_delta->completion);
		wait_for_completion_timeout(&sigma_delta->completion, HZ);

		if (!sigma_delta->irq_dis) {
			disable_irq_nosync(sigma_delta->spi->irq);
			sigma_delta->irq_dis = true;
		}
	}

	sigma_delta->keep_cs_asserted = false;
	ad_sigma_delta_set_mode(sigma_delta, AD_SD_MODE_IDLE);

	sigma_delta->bus_locked = false;
	return spi_bus_unlock(sigma_delta->spi->master);
}

static irqreturn_t ad_sd_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	int ret;

	sigma_delta->current_slot++;

	ret = spi_sync_locked(sigma_delta->spi, &sigma_delta->spi_msg);
	if (ret == 0 && sigma_delta->current_slot == sigma_delta->active_slots) {
		iio_push_to_buffers_with_timestamp(indio_dev,
			sigma_delta->buf_data, pf->timestamp);
		sigma_delta->current_slot = 0;
		sigma_delta->spi_transfer[1].rx_buf = sigma_delta->buf_data;
	} else {
		sigma_delta->spi_transfer[1].rx_buf +=
			indio_dev->channels[0].scan_type.storagebits / 8;
	}

	iio_trigger_notify_done(indio_dev->trig);
	sigma_delta->irq_dis = false;
	enable_irq(sigma_delta->spi->irq);

	return IRQ_HANDLED;
}

static bool ad_sd_validate_scan_mask(struct iio_dev *indio_dev,
	const unsigned long *mask)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);

	return bitmap_weight(mask, indio_dev->masklength) <=
		sigma_delta->num_slots;
}

static const struct iio_buffer_setup_ops ad_sd_buffer_setup_ops = {
	.postenable = &ad_sd_buffer_postenable,
	.postdisable = &ad_sd_buffer_postdisable,
	.validate_scan_mask = &ad_sd_validate_scan_mask,
};

static irqreturn_t ad_sd_data_rdy_trig_poll(int irq, void *private)
{
	struct ad_sigma_delta *sigma_delta = private;

	complete(&sigma_delta->completion);
	disable_irq_nosync(irq);
	sigma_delta->irq_dis = true;
	iio_trigger_poll(sigma_delta->trig);

	return IRQ_HANDLED;
}

/**
 * ad_sd_validate_trigger() - validate_trigger callback for ad_sigma_delta devices
 * @indio_dev: The IIO device
 * @trig: The new trigger
 *
 * Returns: 0 if the 'trig' matches the trigger registered by the ad_sigma_delta
 * device, -EINVAL otherwise.
 */
int ad_sd_validate_trigger(struct iio_dev *indio_dev, struct iio_trigger *trig)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);

	if (sigma_delta->trig != trig)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(ad_sd_validate_trigger);

static const struct iio_trigger_ops ad_sd_trigger_ops = {
	.owner = THIS_MODULE,
};

static int ad_sd_probe_trigger(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	int ret;

	sigma_delta->trig = iio_trigger_alloc("%s-dev%d", indio_dev->name,
						indio_dev->id);
	if (sigma_delta->trig == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}
	sigma_delta->trig->ops = &ad_sd_trigger_ops;
	init_completion(&sigma_delta->completion);

	ret = request_irq(sigma_delta->spi->irq,
			  ad_sd_data_rdy_trig_poll,
			  IRQF_TRIGGER_LOW,
			  indio_dev->name,
			  sigma_delta);
	if (ret)
		goto error_free_trig;

	if (!sigma_delta->irq_dis) {
		sigma_delta->irq_dis = true;
		disable_irq_nosync(sigma_delta->spi->irq);
	}
	sigma_delta->trig->dev.parent = &sigma_delta->spi->dev;
	iio_trigger_set_drvdata(sigma_delta->trig, sigma_delta);

	ret = iio_trigger_register(sigma_delta->trig);
	if (ret)
		goto error_free_irq;

	/* select default trigger */
	indio_dev->trig = iio_trigger_get(sigma_delta->trig);

	return 0;

error_free_irq:
	free_irq(sigma_delta->spi->irq, sigma_delta);
error_free_trig:
	iio_trigger_free(sigma_delta->trig);
error_ret:
	return ret;
}

static void ad_sd_remove_trigger(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);

	iio_trigger_unregister(sigma_delta->trig);
	free_irq(sigma_delta->spi->irq, sigma_delta);
	iio_trigger_free(sigma_delta->trig);
}

/**
 * ad_sd_setup_buffer_and_trigger() -
 * @indio_dev: The IIO device
 */
int ad_sd_setup_buffer_and_trigger(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);
	int ret;

	if (spi_engine_offload_supported(sigma_delta->spi))
		indio_dev->modes |= INDIO_BUFFER_HARDWARE;

	ret = iio_triggered_buffer_setup(indio_dev, &iio_pollfunc_store_time,
			&ad_sd_trigger_handler, &ad_sd_buffer_setup_ops);
	if (ret)
		return ret;

	ret = ad_sd_probe_trigger(indio_dev);
	if (ret) {
		iio_triggered_buffer_cleanup(indio_dev);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ad_sd_setup_buffer_and_trigger);

/**
 * ad_sd_cleanup_buffer_and_trigger() -
 * @indio_dev: The IIO device
 */
void ad_sd_cleanup_buffer_and_trigger(struct iio_dev *indio_dev)
{
	struct ad_sigma_delta *sigma_delta = iio_device_get_drvdata(indio_dev);

	kfree(sigma_delta->buf_data);
	ad_sd_remove_trigger(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);
}
EXPORT_SYMBOL_GPL(ad_sd_cleanup_buffer_and_trigger);

/**
 * ad_sd_init() - Initializes a ad_sigma_delta struct
 * @sigma_delta: The ad_sigma_delta device
 * @indio_dev: The IIO device which the Sigma Delta device is used for
 * @spi: The SPI device for the ad_sigma_delta device
 * @info: Device specific callbacks and options
 *
 * This function needs to be called before any other operations are performed on
 * the ad_sigma_delta struct.
 */
int ad_sd_init(struct ad_sigma_delta *sigma_delta, struct iio_dev *indio_dev,
	struct spi_device *spi, const struct ad_sigma_delta_info *info)
{
	sigma_delta->spi = spi;
	sigma_delta->info = info;
	sigma_delta->num_slots = 1;
	sigma_delta->active_slots = 1;
	iio_device_set_drvdata(indio_dev, sigma_delta);

	return 0;
}
EXPORT_SYMBOL_GPL(ad_sd_init);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("Analog Devices Sigma-Delta ADCs");
MODULE_LICENSE("GPL v2");
