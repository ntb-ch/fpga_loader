/*
 * Copied from fpga.c writen by:
 * Copyright (C) 2006 Luotao Fu <lfu@pengutronix.de>, Pengutronix
 * Copyright (C) 2008 Juergen Beisert <kernel@pengutronix.de>, Pengutronix
 *
 * Modified: 2014 Marco Tinner <marco.tinner@ntb.ch>, NTB Buchs
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
 * The external FPGA device is an Altera Cyclone II, type EP2C8F256C8N
 *
 * It can be feed by an external serial flash device or by some GPIOs from
 * CPU side.
 *
 * Loading from external serial flash at reset time:
 * - in this case PSC6 can be used as an additional UART.
 * - JP11 must be open and R31 assembled
 * - JP8 must be closed at 1-2 to select "Active Serial Mode"
 * - U18 (EPCS4N/25P20) must be assembled
 *
 * Loading from the CPU at runtime:
 * - in this case PSC6 cannot be used as a UART, or if possible, not routed
 *   to the external pins while loading the FPGA
 * - in the case J11 is closed at 1-2
 *    - GPIO6 from CPU must be high
 * - in the case J11 is closed at 3-2
 *    - GPIO6 from CPU is free for alternative usage
 * - J8 must be closed at 2-3 to select "Passive Serial Mode"
 *
 * Now its possible to control:
 *
 * FPGA's             CPUs
 *  pin     through    pin
 * ---------------------------
 *  DCLK (in)        PSC6_3 (out)
 *  DATA0 (in)       PSC6_2 (out)
 *  CONFIG# (in)     PSC6_1 (out)
 *  DONE (out)       PSC6_0 (in)
 *  STATUS (out)     GPIO7 (in)
 *
 *
 * When loading this module the PSC6 is temporarily configured for GPIO use. 
 * From userspace you can use the file copy command to load the FPGA as given in the following example
 *
 * dd if=<design_name>.rbf of=/dev/fpga_loader bs=5M
 *
 * Finally, this module can be unloaded and the UART function will be available again.
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/mpc52xx.h>
#include <linux/slab.h>

#define MAJOR_NR 140
#define MINOR_NR 0
#define N_OF_DEV 1
#define MODULE_NAME "fpga loader"
#define TIMEOUT 20000
#define MAX_FIRMWARE_SIZE 130000


static dev_t dev;
static struct cdev *cdev;
struct class *fpga_loader_class;
static struct mpc52xx_gpio __iomem *gpio;
static struct mpc52xx_gpio_wkup __iomem *wkup;
static int number_of_writes = 0;
static u32 port_config_backup;
static u32 simple_gpioe_backup;
static u32 simple_ddr_backup;
static u8 wkup_gpioe_backup;
static u8 wkup_ddr_backup;
static u8 *buf = NULL;


static void set_dclk(void)
{
	u32 data;

	data = in_be32(&gpio->simple_dvo);
	data |= 0x20000000;
	out_be32(&gpio->simple_dvo, data);
}

static void clear_dclk(void)
{
	u32 data;

	data = in_be32(&gpio->simple_dvo);
	data &= ~0x20000000;
	out_be32(&gpio->simple_dvo, data);
}

static void set_data(void)
{
	u32 data;

	data = in_be32(&gpio->simple_dvo);
	data |= 0x10000000;
	out_be32(&gpio->simple_dvo, data);
}

static void clear_data(void)
{
	u32 data;

	data = in_be32(&gpio->simple_dvo);
	data &= ~0x10000000;
	out_be32(&gpio->simple_dvo, data);
}

static void set_config(void)
{
	u8 config;

	config = in_8(&wkup->wkup_dvo);
	config |= 0x20;
	out_8(&wkup->wkup_dvo, config);
}

static void clear_config(void)
{
	u8 config;

	config = in_8(&wkup->wkup_dvo);
	config &= ~0x20;
	out_8(&wkup->wkup_dvo, config);
}

static int read_done(void)
{
	return (!!(in_8(&wkup->wkup_ival) & 0x10));
}

static int read_status(void)
{
	return (!!(in_8(&wkup->wkup_ival) & 0x80));
}

static void __iomem *mpc52xx_find_and_map(const char *compatible)
{
	struct device_node *ofn;
	const u32 *regaddr_p;
	u64 regaddr64, size64;
	
	ofn = of_find_compatible_node(NULL, NULL, compatible);
	if (!ofn){
		pr_err("%s: of_find_compatible_node error\n", __FUNCTION__);
		return NULL;	
	}

	regaddr_p = of_get_address(ofn, 0, &size64, NULL);
	if (!regaddr_p) {
		pr_err("%s: of_get_address error\n", __FUNCTION__);
		of_node_put(ofn);
		return NULL;
	}

	regaddr64 = of_translate_address(ofn, regaddr_p);

	of_node_put(ofn);

	return ioremap((u32)regaddr64, (u32)size64);
}

static void init_gpio_fpga(void)
{
	u32 port_config, simple_gpioe, simple_ddr;
	u8 wkup_gpioe, wkup_ddr, wkup_dvo;

	/* set port to configure IRDA as GPIO */
	port_config_backup = port_config = in_be32(&gpio->port_config);
	port_config &= ~0x00700000;
	out_be32(&gpio->port_config, port_config);

	/* enabling GPIO_IRDA_0, GPIO_IRDA_1 */
	simple_gpioe = simple_gpioe_backup = in_be32(&gpio->simple_gpioe);
	simple_gpioe |= 0x30000000;
	out_be32(&gpio->simple_gpioe, simple_gpioe);

	/* set GPIO_IRDA_0 as out(Bit 2 low), GPIO_IRDA_1 as out(Bit 3 high)*/
	simple_ddr = simple_ddr_backup = in_be32(&gpio->simple_ddr);
	simple_ddr |= 0x30000000;
	out_be32(&gpio->simple_ddr, simple_ddr);

	/* enabling GPIO_WKUP_4, GPIO_WKUP_5, GPIO_WKUP_6, GPIO_WKUP_7 */
	wkup_gpioe = wkup_gpioe_backup = in_8(&wkup->wkup_gpioe);
	wkup_gpioe |= 0xF0;
	out_8(&wkup->wkup_gpioe, wkup_gpioe);

	/* set GPIO_WKUP_7, GPIO_4 as in (Bit 0 and Bit 3 low) and GPIO_WKUP_5,
	 * GPIO_WKUP_6 as out (Bit 1 and 2 high) */
	wkup_ddr = wkup_ddr_backup = in_8(&wkup->wkup_ddr);
	wkup_ddr &= ~0x90;
	wkup_ddr |= 0x60;
	out_8(&wkup->wkup_ddr, wkup_ddr);

	/* switch the multiplexer to drive the FPGA lines of instead the UART ones */
	wkup_dvo = in_8(&wkup->wkup_dvo);
	wkup_dvo |= 0x40;	/* GPIO6 aka GPIO_WKUP_6 */
	out_8(&wkup->wkup_dvo, wkup_dvo);

	/* set config(GPIO_WKUP_5) to low and wait 50 us till the  */
	/* initialization of the datatransfer */
	clear_config();
	udelay(50);
	/*set config to high and wait till device ready (high on status)*/
	set_config();
}

static void exit_gpio_fpga(void)
{
	u8 wkup_dvo;

	/* switch the multiplexer to drive the UART lines instead of the FPGA ones */
		
	wkup_dvo = in_8(&wkup->wkup_dvo);
	wkup_dvo &= ~0x40;	/* GPIO6 aka GPIO_WKUP_6 */
	out_8(&wkup->wkup_dvo, wkup_dvo);
	
	/* restore previous settings */
	out_8(&wkup->wkup_ddr, wkup_ddr_backup);
	out_8(&wkup->wkup_gpioe, wkup_gpioe_backup);
	out_be32(&gpio->simple_ddr, simple_ddr_backup);
	out_be32(&gpio->simple_gpioe, simple_gpioe_backup);
	out_be32(&gpio->port_config, port_config_backup);

}

static int map_resources(void)
{
	gpio = mpc52xx_find_and_map("fsl,mpc5200-gpio");
	wkup = mpc52xx_find_and_map("fsl,mpc5200-gpio-wkup");

	if(!gpio){
		pr_err("%s: error while mapping GPIO\n", __FUNCTION__);
	}

	if(!wkup){
		pr_err("%s: error while mapping GPIO_WKUP\n", __FUNCTION__);
	}

	if (!gpio || !wkup) {
		return -1;
	}

	return 0;
}

static int unmap_resources(void)
{
	if (gpio) {
		iounmap(gpio);
		gpio = NULL;
	}
	if (wkup) {
		iounmap(wkup);
		wkup = NULL;
	}

	return 0;
}

int open (struct inode *i, struct file *f){
	number_of_writes = 0;
	return 0;
}

int release (struct inode *i, struct file *f){
	return 0;
}

ssize_t write(struct file *f, const char __user *data, size_t file_size, loff_t *offs ){
	int bits_transferred = 0, size, timeout = TIMEOUT;
	u8 conf_done, status;
	number_of_writes++;
	if(file_size > MAX_FIRMWARE_SIZE){
		printk(KERN_ALERT "[%s]: Error File is to big!\n", MODULE_NAME);
		return -EFBIG;
	}

	if(number_of_writes >1){
		printk(KERN_ALERT "[%s]: Error File has to be writen in one chunk!\n For example use: dd if=.. of=.. bs=5M\n", MODULE_NAME);
		return -EIO;
	}
	memcpy(buf,data,file_size);
	size = file_size;
	while (1) {
		status = read_status();
		if (status)
			break;
		if (timeout <= 0) {
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
		exit_gpio_fpga();
		unmap_resources();
	map_res_fail:
	return error;	

}

static void __exit fpga_exit(void)
{
	kfree(buf);
	exit_gpio_fpga();
	unmap_resources();
	device_destroy(fpga_loader_class, dev);
	class_destroy(fpga_loader_class);
	cdev_del(cdev);
	unregister_chrdev_region(dev, N_OF_DEV);
	printk(KERN_DEBUG "[%s]: successfully unloaded\n", MODULE_NAME);
}

module_init(fpga_init);
module_exit(fpga_exit);
MODULE_AUTHOR("Luotao Fu, Juergen Beisert, Tinner Marco");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FPGA Loader on pcm032 Board");

