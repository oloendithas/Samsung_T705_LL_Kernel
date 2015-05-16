/* linux/drivers/video/mdnie_tuning.c
 *
 * Register interface file for Samsung mDNIe driver
 *
 * Copyright (c) 2011 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/lcd.h>
#include <linux/rtc.h>
#include <linux/fb.h>
#include <mach/gpio.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/magic.h>
#include <linux/mdnie.h>

#include "mdnie_lite.h"

#ifdef CONFIG_LCD_MIPI_S6TNMR7
#define ADDR_SIGNATURE	0xBA
#else
#define ADDR_SIGNATURE	0xEC
#endif

#define BRIGHTNESS_RANGE 254

static struct mdnie_table *tuning_table;
static struct mdnie_table *correction_table;

void save_tuning_table(struct mdnie_table *table) {
	tuning_table = table;
}

struct mdnie_table *get_tuning_table(void) {
	return tuning_table;
}

void save_correction_table(struct mdnie_table *table) {
	correction_table = table;
}

struct mdnie_table *get_correction_table(void) {
	return correction_table;
}

int mdnie_calibration(int *r)
{
	int ret = 0;

	if (r[1] > 0) {
		if (r[3] > 0)
			ret = 3;
		else
			ret = (r[4] < 0) ? 1 : 2;
	} else {
		if (r[2] < 0) {
			if (r[3] > 0)
				ret = 9;
			else
				ret = (r[4] < 0) ? 7 : 8;
		} else {
			if (r[3] > 0)
				ret = 6;
			else
				ret = (r[4] < 0) ? 4 : 5;
		}
	}

	ret = (ret > 0) ? ret : 1;
	ret = (ret > 9) ? 1 : ret;

	pr_info("mdnie_calibration: %d, %d, %d, %d, tune%d\n", r[1], r[2], r[3], r[4], ret);

	return ret;
}

int mdnie_open_file(const char *path, char **fp)
{
	struct file *filp;
	char	*dp;
	long	length;
	int     ret;
	struct super_block *sb;
	loff_t  pos = 0;

	if (!path) {
		pr_err("%s: path is invalid\n", __func__);
		return -EPERM;
	}

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("%s: filp_open skip: %s\n", __func__, path);
		return -EPERM;
	}

	length = i_size_read(filp->f_path.dentry->d_inode);
	if (length <= 0) {
		pr_err("%s: file length is %ld\n", __func__, length);
		return -EPERM;
	}

	dp = kzalloc(length, GFP_KERNEL);
	if (dp == NULL) {
		pr_err("%s: fail to alloc size %ld\n", __func__, length);
		filp_close(filp, current->files);
		return -EPERM;
	}

	ret = kernel_read(filp, pos, dp, length);
	if (ret != length) {
		/* check node is sysfs, bus this way is not safe */
		sb = filp->f_path.dentry->d_inode->i_sb;
		if ((sb->s_magic != SYSFS_MAGIC) && (length != sb->s_blocksize)) {
			pr_err("%s: read size= %d, length= %ld\n", __func__, ret, length);
			kfree(dp);
			filp_close(filp, current->files);
			return -EPERM;
		}
	}

	filp_close(filp, current->files);

	*fp = dp;

	return ret;
}

int mdnie_check_firmware(const char *path, char *name)
{
	char *ptr = NULL;
	int ret = 0, size;

	size = mdnie_open_file(path, &ptr);
	if (IS_ERR_OR_NULL(ptr) || size <= 0) {
		pr_err("%s: file open skip %s\n", __func__, path);
		kfree(ptr);
		return -EPERM;
	}

	ret = (strstr(ptr, name) != NULL) ? 1 : 0;

	kfree(ptr);

	return ret;
}

static int mdnie_request_firmware(char *path, char *name, mdnie_t **buf)
{
	char *token, *ptr = NULL, *ptr_save;
	int ret = 0, size;
	unsigned int data[2], i = 0, j;
	mdnie_t *dp;

	size = mdnie_open_file(path, &ptr);
	if (IS_ERR_OR_NULL(ptr) || size <= 0) {
		pr_err("%s: file open skip %s\n", __func__, path);
		kfree(ptr);
		return -EPERM;
	}

	dp = kzalloc(size, GFP_KERNEL);
	if (dp == NULL) {
		pr_err("%s: fail to alloc size %d\n", __func__, size);
		kfree(ptr);
		return -ENOMEM;
	}

	ptr_save = ptr;
	if (name) {
		if (strstr(ptr, name) != NULL) {
			pr_info("found %s in %s\n", name, path);
			ptr = strstr(ptr, name);
		}
	}

	if (strsep(&ptr, "{") == NULL) {
		pr_err("%s: fail to find { key \n", __func__);
		kfree(ptr_save);
		return -EINVAL;
	}

	while ((token = strsep(&ptr, "\r\n")) != NULL) {
		if (name && !strncmp(token, "};", 2)) {
			pr_info("found %s end in local, stop searching\n", name);
			break;
		}
		ret = sscanf(token, "%x,", &data[0]);
		for (j = 0; j < ret; i++, j++)
			dp[i] = data[j];
	}

	*buf = dp;

	kfree(ptr_save);

	return i;
}

struct mdnie_table *mdnie_request_table(char *path, struct mdnie_table *s)
{
	char string[50];
	unsigned int i, j, size;
	mdnie_t *buf = NULL;
	struct mdnie_table *t;
	int ret = 0;

	ret = mdnie_check_firmware(path, s->name);
	if (ret < 0)
		return NULL;

	t = kzalloc(sizeof(struct mdnie_table), GFP_KERNEL);
	t->name = kzalloc(strlen(s->name) + 1, GFP_KERNEL);
	strcpy(t->name, s->name);
	memcpy(t, s, sizeof(struct mdnie_table));

	if (ret > 0) {
		for (j = 1, i = MDNIE_CMD1; i <= MDNIE_CMD2; i++, j++) {
			memset(string, 0, sizeof(string));
			sprintf(string, "%s_%d", t->name, j);
			size = mdnie_request_firmware(path, string, &buf);
			t->tune[i].sequence = buf;
			t->tune[i].size = size;
			pr_info("%s: size is %d\n", string, t->tune[i].size);
		}
	} else if (ret == 0) {
		size = mdnie_request_firmware(path, NULL, &buf);

		if (size <= 0)
			return t;

		/* skip first byte for check signature */
		for (i = 1; i < size; i++) {
			if (buf[i] == ADDR_SIGNATURE)
				break;
		}

		t->tune[MDNIE_CMD1].sequence = &buf[0];
		t->tune[MDNIE_CMD1].size = i;
		t->tune[MDNIE_CMD2].sequence = &buf[i];
		t->tune[MDNIE_CMD2].size = size - i + 1;
	}

	return t;
}

static unsigned int int_sqrt_custom(unsigned int x) {
	int v = x, z = 1, y = 0, i = 1; 
	
	while (v > 0) {
		v -= z; 
		if (v > 0 || v + z > v * -1)
			y++; 
		z += 2; 
	}
	
	return y;
}

static unsigned int int_cbrt_custom(unsigned int x) {
	int z, y = x / 3;
	
	if (y == 0)
		y = 1;
			
	while (y - z > 1)
	{
		z = int_sqrt_custom(x / y);
		y = (y + z + z) / 3;
	}
	
	return y;
};

// precalculated curved coefficients [(255 - curr_bright) / 254 * (1 / sqrt(curr_bright)) * 1000]
static int brightnes_correct_coeff[] = {
	1000, 1000, 704, 573, 494, 440, 400, 369, 344, 323, 305, 290, 276, 264, 254, 244, 235, 227, 220, 213, 207, 201, 196, 190, 186, 
	181, 177, 173, 169, 165, 162, 158, 155, 152, 149, 146, 144, 141, 139, 136, 134, 132, 129, 127, 125, 123, 121, 119, 118, 
	116, 114, 112, 111, 109, 108, 106, 105, 103, 102, 100, 99, 98, 97, 95, 94, 93, 92, 90, 89, 88, 87, 86, 85, 84, 83, 82, 
	81, 80, 79, 78, 77, 76, 75, 74, 73, 73, 72, 71, 70, 69, 68, 68, 67, 66, 65, 65, 64, 63, 62, 62, 61, 60, 60, 59, 58, 
	58, 57, 56, 56, 55, 54, 54, 53, 53, 52, 51, 51, 50, 50, 49, 49, 48, 47, 47, 46, 46, 45, 45, 44, 44, 43, 43, 42, 42, 
	41, 41, 40, 40, 39, 39, 38, 38, 37, 37, 36, 36, 36, 35, 35, 34, 34, 33, 33, 32, 32, 32, 31, 31, 30, 30, 30, 29, 29, 
	28, 28, 28, 27, 27, 26, 26, 26, 25, 25, 25, 24, 24, 23, 23, 23, 22, 22, 22, 21, 21, 21, 20, 20, 20, 19, 19, 19, 18, 
	18, 18, 17, 17, 17, 16, 16, 16, 15, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 11, 11, 11, 10, 10, 10, 10, 9, 
	9, 9, 8, 8, 8, 8, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 1, 1, 1, 1, 0, 0
};

static int last_brightness;
void mdnie_update_brightness(struct mdnie_device *md_dev, int brightness)
{
	struct mdnie_info *mdnie;
	struct mdnie_table *table = NULL;
	int i = 0, j = 0;

	if (brightness && brightness > 0)
		last_brightness = brightness - 1;

	if (get_tuning_enabled() != 1) {
#ifdef DEBUG		
		pr_debug("tuning_enabled != 1\n");
#endif
		return;
	}

	if (IS_ERR_OR_NULL(md_dev)) {
		pr_err("md_dev is NULL\n");
		return;
	}
	
	mdnie = (struct mdnie_info *)mdnie_get_data(md_dev);
	
	if (IS_ERR_OR_NULL(mdnie)) {
		pr_err("mdnie is NULL\n");
		return;
	}
	
	if (!mdnie->enable) {
#ifdef DEBUG		
		dev_dbg(mdnie->dev, "%s: mdnie state is off\n", __func__);
#endif
		return;
	}

	if (!mdnie->tuning) {
#ifdef DEBUG		
		dev_dbg(mdnie->dev, "%s: tunning mode is off\n", __func__);
#endif
		return;
	}
    
	if (IS_ERR_OR_NULL(correction_table)) {
#ifdef DEBUG		
		dev_dbg(mdnie->dev, "%s: correction_table is NULL\n", __func__);
#endif
		return;
	}
	
#ifdef DEBUG		
	if (!mdnie->path) {
		dev_dbg(mdnie->dev, "%s: mdnie->path is not set\n", __func__);
	} else {
		dev_dbg(mdnie->dev, "%s: mdnie->path=%s\n", __func__, mdnie->path);
	}
#endif
 
	table = mdnie_find_table(mdnie);
	
	if (IS_ERR_OR_NULL(table)) {
		dev_err(mdnie->dev, "%s: mdnie table is NULL\n", __func__);
		return;
	}
	
	if (IS_ERR_OR_NULL(table->name)) {
		dev_err(mdnie->dev, "%s: table->name is NULL\n", __func__);
		return;
	} 
#ifdef DEBUG		
	else {
		dev_dbg(mdnie->dev, "%s: table->name=%s\n", __func__, table->name);
	}

	dev_dbg(mdnie->dev, "table->tune[MDNIE_CMD1].size=%d, table->tune[MDNIE_CMD2].size=%d\n",
		table->tune[MDNIE_CMD1].size, table->tune[MDNIE_CMD2].size);
#endif
		
	for (j = MDNIE_CMD1; j <= MDNIE_CMD2; j++) {
		for (i = 1; i < table->tune[j].size; i++) {
			if (table->tune[j].sequence[i] != tuning_table->tune[j].sequence[i])
				table->tune[j].sequence[i] = tuning_table->tune[j].sequence[i];
				
			if (correction_table->tune[j].sequence[i] != 0) {
				int correction, corrected, newval;
				
				correction = correction_table->tune[j].sequence[i];
				if (correction > 127)
					correction -= 256;
					
				corrected = correction * brightnes_correct_coeff[last_brightness] / 1000;
				newval = table->tune[j].sequence[i] + corrected;
							
				if (newval > 255)
					newval = 255;
				if (newval < 0)
					newval = 0;
				
#ifdef DEBUG		
				dev_dbg(mdnie->dev, "j=%d, i=%d, brightness=%d, current=%d, correct=%d, brightnes_correct_coeff=%d, corrected=%d, newval=%d\n",
							j, i, last_brightness, table->tune[j].sequence[i], correction, brightnes_correct_coeff[last_brightness], corrected, newval); 
#endif
				
				table->tune[j].sequence[i] = newval;
			}
		}
	}
	mdnie_write_table(mdnie, table);
}
