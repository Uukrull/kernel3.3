/* Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
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
/* The NVS = NVidia Sensor framework */
/* This common NVS ALS module allows, along with the NVS IIO common module, an
 * ALS driver to offload the code interacting with IIO and ALS reporting, and
 * just have code that interacts with the HW.
 * The commonality between this module and the NVS ALS driver is the nvs_light
 * structure.  It is expected that the NVS ALS driver will:
 * - call nvs_light_enable when the device is enabled to initialize variables.
 * - read the HW and place the value in nvs_light.hw
 * - call nvs_light_read
 * - depending on the nvs_light_read return value:
 *     - -1 = poll HW using nvs_light.poll_delay_ms delay.
 *     - 0 = if interrupt driven, do nothing or resume regular polling
 *     - 1 = set new thresholds using the nvs_light.hw_thresh_lo/hi
 * Reporting the lux is handled within this module.
 * See nvs_light.h for nvs_light structure details.
 */
/* The NVS HAL will use the IIO scale and offset sysfs attributes to modify the
 * data using the following formula: (data * scale) + offset
 * A scale value of 0 disables scale.
 * A scale value of 1 puts the NVS HAL into calibration mode where the scale
 * and offset are read everytime the data is read to allow realtime calibration
 * of the scale and offset values to be used in the device tree parameters.
 * Keep in mind the data is buffered but the NVS HAL will display the data and
 * scale/offset parameters in the log.  See calibration steps below.
 */
/* The configuration threshold values are HW value based.  In other words, to
 * obtain the upper and lower HW thresholds, the configuration threshold is
 * simply added or subtracted from the HW data read, respectively.
 * A little history about this: this code originally expected the configuration
 * threshold values to be in lux.  It then converted the threshold lux value to
 * a HW value by reversing the calibrated value to uncalibrated and then the
 * scaling of the resolution.  The idea was to make configuration easy by just
 * setting how much lux needed to change before reporting.  However, there were
 * two issues with this method:
 * 1) that's a lot of overhead for each sample, and 2) the lux range isn't
 * exactly linear.  Lux values in a dark room will probably want to be reported
 * every +/- 10 or 100 if not less.  This is opposed to a bright room or even
 * outside on a sunny day where lux value changes can be reported every
 * +/- 1000 or even 10000.  Since many ALS's have dynamic resolution, changing
 * the range depending on the lux reading, it makes sense to use HW threshold
 * values that will automatically scale with the HW resolution used.
 */
/* NVS light drivers have two calibration mechanisms:
 * Method 1 (preferred):
 * This method uses interpolation and requires a low and high uncalibrated
 * value along with the corresponding low and high calibrated values.  The
 * uncalibrated values are what is read from the sensor in the steps below.
 * The corresponding calibrated values are what the correct value should be.
 * All values are programmed into the device tree settings.
 * 1. Read scale sysfs attribute.  This value will need to be written back.
 * 2. Disable device.
 * 3. Write 1 to the scale sysfs attribute.
 * 4. Enable device.
 * 5. The NVS HAL will announce in the log that calibration mode is enabled and
 *    display the data along with the scale and offset parameters applied.
 * 6. Write the scale value read in step 1 back to the scale sysfs attribute.
 * 7. Put the device into a state where the data read is a low value.
 * 8. Note the values displayed in the log.  Separately measure the actual
 *    value.  The value from the sensor will be the uncalibrated value and the
 *    separately measured value will be the calibrated value for the current
 *    state (low or high values).
 * 9. Put the device into a state where the data read is a high value.
 * 10. Repeat step 8.
 * 11. Enter the values in the device tree settings for the device.  Both
 *     calibrated and uncalibrated values will be the values before scale and
 *     offset are applied.
 *     The light sensor has the following device tree parameters for this:
 *     light_uncalibrated_lo
 *     light_calibrated_lo
 *     light_uncalibrated_hi
 *     light_calibrated_hi
 *
 * Method 2:
 * 1. Disable device.
 * 2. Write 1 to the scale sysfs attribute.
 * 3. Enable device.
 * 4. The NVS HAL will announce in the log that calibration mode is enabled and
 *    display the data along with the scale and offset parameters applied.
 * 5. Write to scale and offset sysfs attributes as needed to get the data
 *    modified as desired.
 * 6. Disabling the device disables calibration mode.
 * 7. Set the new scale and offset parameters in the device tree:
 *    light_scale_ival = the integer value of the scale.
 *    light_scale_fval = the floating value of the scale.
 *    light_offset_ival = the integer value of the offset.
 *    light_offset_fval = the floating value of the offset.
 *    The values are in the NVS_SCALE_SIGNIFICANCE format (see nvs.h).
 */
/* The reason calibration method 1 is preferred is that the NVS ALS driver
 * already sets the scaling to coordinate with the resolution by multiplying
 * the HW data value read with resolution * scaling and then divides it back
 * down with the scaling so that no significance is lost.
 */


#include <linux/of.h>
#include <linux/nvs_light.h>


static unsigned int nvs_light_interpolate(int x1, s64 x2, int x3,
					  int y1, int y3)
{
	s64 dividend;
	s64 divisor;

	/* y2 = ((x2 - x1)(y3 - y1)/(x3 - x1)) + y1 */
	divisor = (x3 - x1);
	if (!divisor)
		return (unsigned int)x2;

	dividend = (x2 - x1) * (y3 - y1);
	do_div(dividend, divisor);
	dividend += y1;
	if (dividend < 0)
		dividend = 0;
	return (unsigned int)dividend;
}

static int nvs_light_nld(struct nvs_light *nl, unsigned int nld_i)
{
	nl->nld_i = nld_i;
	nl->nld_i_change = true;
	nl->cfg->resolution.ival = nl->nld_tbl[nld_i].resolution.ival;
	nl->cfg->resolution.fval = nl->nld_tbl[nld_i].resolution.fval;
	nl->cfg->max_range.ival = nl->nld_tbl[nld_i].max_range.ival;
	nl->cfg->max_range.fval = nl->nld_tbl[nld_i].max_range.fval;
	nl->cfg->milliamp.ival = nl->nld_tbl[nld_i].milliamp.ival;
	nl->cfg->milliamp.fval = nl->nld_tbl[nld_i].milliamp.fval;
	nl->cfg->delay_us_min = nl->nld_tbl[nld_i].delay_min_ms * 1000;
	return RET_POLL_NEXT;
}

/**
 * nvs_light_read - called after HW is read and placed in nl.
 * @nl: the common structure between driver and common module.
 *
 * This will handle the conversion of HW to lux value,
 * reporting, calculation of thresholds and poll time.
 *
 * Returns: -1 = Error and/or polling is required for next
 *               sample regardless of being interrupt driven.
 *          0 = Do nothing.  Lux has not changed for reporting
 *              and same threshold values if interrupt driven.
 *              If not interrupt driven use poll_delay_ms.
 *          1 = New HW thresholds are needed.
 *              If not interrupt driven use poll_delay_ms.
 */
int nvs_light_read(struct nvs_light *nl)
{
	u64 calc_i;
	u64 calc_f;
	s64 calc;
	s64 timestamp_diff;
	s64 delay;
	bool report_delay_min = true;
	unsigned int poll_delay = 0;
	unsigned int thresh_lo;
	unsigned int thresh_hi;
	int ret;

	if (nl->calibration_en)
		/* always report without report_delay_min */
		nl->report = nl->cfg->report_n;
	if (nl->report < nl->cfg->report_n) { /* always report first sample */
		/* calculate elapsed time for allowed report rate */
		timestamp_diff = nl->timestamp - nl->timestamp_report;
		delay = nl->delay_us * 1000;
		if (timestamp_diff < delay) {
			/* data changes are happening faster than allowed to
			 * report so we poll for the next data at an allowed
			 * rate with interrupts disabled.
			 */
			delay -= timestamp_diff;
			do_div(delay, 1000); /* ns => us */
			poll_delay = delay;
			report_delay_min = false;
		}
	}
	/* threshold flags */
	thresh_lo = nl->cfg->thresh_lo;
	thresh_hi = nl->cfg->thresh_lo;
	if (thresh_lo < nl->hw_mask) {
		nl->thresh_valid_lo = true;
	} else {
		nl->thresh_valid_lo = false;
		thresh_lo = 0;
	}
	if (thresh_hi < nl->hw_mask) {
		nl->thresh_valid_hi = true;
	} else {
		nl->thresh_valid_hi = false;
		thresh_hi = 0;
	}
	if (nl->thresh_valid_lo && nl->thresh_valid_hi)
		nl->thresholds_valid = true;
	else
		nl->thresholds_valid = false;
	/* limit flags */
	if ((nl->hw < thresh_lo) || (nl->hw == 0))
		nl->hw_limit_lo = true;
	else
		nl->hw_limit_lo = false;
	if ((nl->hw == nl->hw_mask) || (nl->hw > (nl->hw_mask - thresh_hi)))
		nl->hw_limit_hi = true;
	else
		nl->hw_limit_hi = false;
	/* reporting and thresholds */
	if (nl->nld_i_change) {
		/* HW resolution just changed.  Need thresholds and reporting
		 * based on new settings.  Reporting may not be this cycle due
		 * to report_delay_min.
		 */
		nl->report = nl->cfg->report_n;
	} else {
		if (nl->thresholds_valid) {
			if (nl->hw < nl->hw_thresh_lo)
				nl->report = nl->cfg->report_n;
			else if (nl->hw > nl->hw_thresh_hi)
				nl->report = nl->cfg->report_n;
		} else {
			/* report everything if no thresholds */
			nl->report = nl->cfg->report_n;
		}
	}
	ret = RET_NO_CHANGE;
	/* lux reporting */
	if (nl->report && report_delay_min) {
		nl->report--;
		nl->timestamp_report = nl->timestamp;
		/* lux = HW * (resolution * NVS_SCALE_SIGNIFICANCE) / scale */
		calc_f = 0;
		if (nl->cfg->resolution.fval) {
			calc_f = (u64)(nl->hw * nl->cfg->resolution.fval);
			if (nl->cfg->scale.fval)
				do_div(calc_f, nl->cfg->scale.fval);
		}
		calc_i = 0;
		if (nl->cfg->resolution.ival) {
			calc_i = NVS_SCALE_SIGNIFICANCE / nl->cfg->scale.fval;
			calc_i *= (u64)(nl->hw * nl->cfg->resolution.ival);
		}
		calc = (s64)(calc_i + calc_f);
		/* get calibrated value */
		nl->lux = nvs_light_interpolate(nl->cfg->uncal_lo, calc,
						nl->cfg->uncal_hi,
						nl->cfg->cal_lo,
						nl->cfg->cal_hi);
		/* report lux */
		nl->handler(nl->nvs_data, &nl->lux, nl->timestamp_report);
		if ((nl->thresholds_valid) && !nl->report) {
			/* calculate low threshold */
			calc = (s64)nl->hw;
			calc -= thresh_lo;
			if (calc < 0)
				/* low threshold is disabled */
				nl->hw_thresh_lo = 0;
			else
				nl->hw_thresh_lo = calc;
			/* calculate high threshold */
			calc = nl->hw + thresh_hi;
			if (calc > nl->hw_mask)
				/* high threshold is disabled */
				nl->hw_thresh_hi = nl->hw_mask;
			else
				nl->hw_thresh_hi = calc;
			ret = RET_HW_UPDATE;
		}
	}
	/* dynamic resolution */
	nl->nld_i_change = false;
	if (nl->nld_tbl) { /* if dynamic resolution is enabled */
		/* adjust resolution if need to make room for thresholds */
		if (nl->hw_limit_hi && (nl->nld_i < nl->nld_i_hi))
			/* too many photons - need to increase resolution */
			ret = nvs_light_nld(nl, nl->nld_i + 1);
		else if (nl->hw_limit_lo && (nl->nld_i > nl->nld_i_lo))
			/* not enough photons - need to decrease resolution */
			ret = nvs_light_nld(nl, nl->nld_i - 1);
	}
	/* poll time */
	if (nl->nld_i_change) {
		nl->poll_delay_ms = nl->nld_tbl[nl->nld_i].delay_min_ms;
	} else {
		if (report_delay_min)
			poll_delay = nl->delay_us;
		if ((poll_delay < nl->cfg->delay_us_min) || nl->calibration_en)
			poll_delay = nl->cfg->delay_us_min;
		nl->poll_delay_ms = poll_delay / 1000;
	}
	if (nl->report || nl->calibration_en)
		ret = RET_POLL_NEXT; /* poll for next sample */
	return ret;
}

/**
 * nvs_light_enable - called when the light sensor is enabled.
 * @nl: the common structure between driver and common module.
 *
 * This inititializes the nl NVS variables.
 *
 * Returns 0 on success or a negative error code.
 */
int nvs_light_enable(struct nvs_light *nl)
{
	if (!nl->cfg->report_n)
		nl->cfg->report_n = 1;
	nl->report = nl->cfg->report_n;
	nl->timestamp_report = 0;
	nl->hw_thresh_hi = 0;
	nl->hw_thresh_lo = -1;
	if (nl->nld_tbl)
		nvs_light_nld(nl, nl->nld_i_hi);
	else
		nl->poll_delay_ms = nl->cfg->delay_us_min / 1000;
	if (nl->cfg->scale.ival == 1 && !nl->cfg->scale.fval)
		nl->calibration_en = true;
	else
		nl->calibration_en = false;
	return 0;
}

/**
 * nvs_light_of_dt - called during system boot to acquire
 * dynamic resolution table index limits.
 * @nl: the common structure between driver and common module.
 * @np: device node pointer.
 * @dev_name: device name string.  Typically a string to "light"
 *            or NULL.
 *
 * Returns 0 on success or a negative error code.
 *
 * Driver must initialize variables if no success.
 * NOTE: DT must have both indexes for a success.
 */
int nvs_light_of_dt(struct nvs_light *nl, const struct device_node *np,
		    const char *dev_name)
{
	char str[256];
	int ret;
	int ret_t = -EINVAL;

	if (np == NULL)
		return -EINVAL;

	if (dev_name == NULL)
		dev_name = NVS_LIGHT_STRING;
	ret = sprintf(str, "%s_dynamic_resolution_index_limit_low", dev_name);
	if (ret > 0)
		ret_t = of_property_read_u32(np, str, &nl->nld_i_lo);
	ret = sprintf(str, "%s_dynamic_resolution_index_limit_high", dev_name);
	if (ret > 0)
		ret_t |= of_property_read_u32(np, str, &nl->nld_i_hi);
	if (nl->nld_i_hi < nl->nld_i_lo)
		return -EINVAL;

	return ret_t;
}
