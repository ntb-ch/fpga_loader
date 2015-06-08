/*
 * Copied from fpga.c writen by:
 * Copyright (C) 2006 Luotao Fu <lfu@pengutronix.de>, Pengutronix
 * Copyright (C) 2008 Juergen Beisert <kernel@pengutronix.de>, Pengutronix
 *
 * Modified: 2014 Marco Tinner <marco.tinner@ntb.ch>, NTB Buchs
 *           2015 Adam Bajric <adam.bajric@ntb.ch>, NTB Buchs
 *
 * This programs free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Note:
 * The external FPGA device is an Altera Cyclone IV, type EP4CE22F17C8
 *
 * It can be feed by an external serial flash device or by some GPIOs from
 * CPU side.
 *
 * Now its possible to control:
 *
 * FPGA's             CPUs
 *  pin     through    pin
 * ---------------------------
 *  DCLK (in)        FPGA_CONFIG_DCLK    (out) bank 2 bit 31
 *  DATA0 (in)       FPGA_CONFIG_DATA0   (out) bank 3 bit 29
 *  CONFIG# (in)     FPGA_CONFIG_nCONFIG (out) bank 5 bit  4
 *  DONE (out)       FPGA_CONFIG_DONE    (in)  bank 1 bit 11
 *  STATUS (out)     FPGA_CONFIG_nSTATUS (in)  bank 3 bit 27
 *
 *
 * From userspace you can use the file copy command to load the FPGA as given in the following example
 *
 * dd if=<design_name>.rbf of=/dev/fpga_loader bs=5M
 *
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>


#define MAJOR_NR 140
#define MINOR_NR 0
#define N_OF_DEV 1
#define MODULE_NAME "fpga loader"
#define TIMEOUT 20000
#define MAX_FIRMWARE_SIZE 4194304


static dev_t dev;
static struct cdev *cdev;
struct class *fpga_loader_class;
static int number_of_writes = 0;
static u8 *buf = NULL;

enum
{
	FPGA_CONFIG_nCONFIG = 63,
	FPGA_CONFIG_nSTATUS = 93,
	FPGA_CONFIG_DCLK = 132,
	FPGA_CONFIG_DATA0 = 11,
	FPGA_CONFIG_DONE = 91
};

static struct gpio fpga_gpios[] =
{
	{ FPGA_CONFIG_nCONFIG, GPIOF_OUT_INIT_HIGH, "nCONFIG" },
	{ FPGA_CONFIG_nSTATUS, GPIOF_IN,            "nSTATUS" },
	{ FPGA_CONFIG_DCLK,    GPIOF_OUT_INIT_LOW,  "DCLK"    },
	{ FPGA_CONFIG_DATA0,   GPIOF_OUT_INIT_LOW,  "DATA0"   },
	{ FPGA_CONFIG_DONE,    GPIOF_IN,            "DONE"    }
};


static void set_dclk(void)     { gpio_set_value(FPGA_CONFIG_DCLK, 1); }
static void clear_dclk(void)   { gpio_set_value(FPGA_CONFIG_DCLK, 0); }

static void set_data(void)     { gpio_set_value(FPGA_CONFIG_DATA0, 1); }
static void clear_data(void)   { gpio_set_value(FPGA_CONFIG_DATA0, 0); }

static void set_config(void)   { gpio_set_value(FPGA_CONFIG_nCONFIG, 1); }
static void clear_config(void) { gpio_set_value(FPGA_CONFIG_nCONFIG, 0); }

static int read_done(void)     { return gpio_get_value(FPGA_CONFIG_DONE); }

static int read_status(void)   { return gpio_get_value(FPGA_CONFIG_nSTATUS); }


static void init_gpio_fpga(void)
{
	clear_config();
	udelay(50);
	/*set config to high and wait till device ready (high on status)*/
	set_config();
}

static int map_resources(void)
{
	return gpio_request_array(fpga_gpios, ARRAY_SIZE(fpga_gpios));
}

static void unmap_resources(void)
{
	gpio_free_array(fpga_gpios, ARRAY_SIZE(fpga_gpios));
}

int open (struct inode *i, struct file *f)
{
	number_of_writes = 0;

	return 0;
}

int release (struct inode *i, struct file *f){
	return 0;
}

ssize_t write(struct file *f, const char __user *data, size_t file_size, loff_t *offs ){
	int bits_transferred = 0, size, timeout = TIMEOUT;
	u8 conf_done;
	number_of_writes++;
	if(file_size > MAX_FIRMWARE_SIZE){
		printk(KERN_ALERT "[%s]: Error File is too big!\n", MODULE_NAME);
		return -EFBIG;
	}

	if(number_of_writes >1){
		printk(KERN_ALERT "[%s]: Error File has to be writen in one chunk!\n For example use: dd if=.. of=.. bs=5M\n", MODULE_NAME);
		return -EIO;
	}
	memcpy(buf,data,file_size);
	size = file_size;
	while (1) {
		if (read_status())
			break;
		if (timeout <= 0) {
			printk(KERN_ALERT "[%s]: timeout\n", MODULE_NAME);
			return -ENODEV;
		}
		timeout--;
		udelay(1);
	}

	printk(KERN_DEBUG "[%s]: flashing firmware, %d bytes to go.\n",MODULE_NAME, size);


	//set nConfig to low and bring it back to high to indicate reconfig
	clear_config();
	udelay(1000);
	set_config();
	udelay(1000);

	while (1) {
		conf_done = read_done();
		if (conf_done)
			break;

		if (*buf & 0x1)
			set_data();
		else
			clear_data();

		set_dclk();
		clear_dclk();

		*buf >>= 1;

		if (!(++bits_transferred % 8))
			buf++;

		if(!(bits_transferred % 8192))
			printk(".");

		if (bits_transferred > size * 8){
			printk(KERN_DEBUG "\n[%s]: warning: bit overrun \n", MODULE_NAME);
			break;
		}
	}
	printk(KERN_DEBUG "\n[%s]: done \n", MODULE_NAME);
	printk(KERN_DEBUG "[%s]: config done status: %i \n", MODULE_NAME,conf_done ? 1:0);
	printk(KERN_DEBUG "[%s]: transferred bits: %d \n", MODULE_NAME,bits_transferred);
	return size;
	
}

struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = open,
	.write = write,
	.release = release,
};

static int __init fpga_init(void)
{
	int error = 0;

	error = map_resources();
	if(error){
		printk(KERN_ALERT "[%s]: Map resources failed\n", MODULE_NAME);
		goto map_res_fail;
	}
	init_gpio_fpga();

	 //register char devices
	dev= MKDEV(MAJOR_NR,MINOR_NR);
	error = register_chrdev_region(dev,N_OF_DEV,MODULE_NAME);
	if(error){
		printk(KERN_ALERT "[%s]: Error add c_dev failed\n", MODULE_NAME);
		goto reg_dev_reg_fail;
	}

	cdev = cdev_alloc();
	cdev->owner=THIS_MODULE;
	cdev_init(cdev,&fops);
	error = cdev_add(cdev,dev,1);
	if(error) goto cdev_add_fail;
	
	fpga_loader_class = class_create(THIS_MODULE, "fpga_loader");
	device_create(fpga_loader_class, NULL, dev, NULL, "fpga_loader");
	buf = kmalloc(MAX_FIRMWARE_SIZE, GFP_USER);
	printk(KERN_DEBUG "[%s]: successfully loaded\n", MODULE_NAME);
	return 0;
	
	cdev_add_fail:
		unregister_chrdev_region(dev, N_OF_DEV);
	reg_dev_reg_fail:
		unmap_resources();
	map_res_fail:
	return error;	

}

static void __exit fpga_exit(void)
{
	kfree(buf);
	unmap_resources();
	device_destroy(fpga_loader_class, dev);
	class_destroy(fpga_loader_class);
	cdev_del(cdev);
	unregister_chrdev_region(dev, N_OF_DEV);
	printk(KERN_DEBUG "[%s]: successfully unloaded\n", MODULE_NAME);
}

module_init(fpga_init);
module_exit(fpga_exit);
MODULE_AUTHOR("Luotao Fu, Juergen Beisert, Tinner Marco, Adam Bajric");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FPGA Loader on iMX6");

