/*
 * NES, SNES, gamepad driver for Raspberry Pi
 *
 *  Copyright (c) 2014	Christian Isaksson
 *  Copyright (c) 2014	Karl Thoren <karl.h.thoren@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <asm/io.h>

/* _____ _____ _____ ____
  / ____|  __ \_   _/ __ \ 
 | |  __| |__) || || |  | |
 | | |_ |  ___/ | || |  | |
 | |__| | |    _| || |__| |
  \_____|_|   |_____\____/                            
*/

static volatile unsigned *gpio;	// I/O access.

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))	// Set GPIO as input.
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))	// Set GPIO as output.

#define GPIO_SET *(gpio + 7)	// Sets bits which are 1 and ignores bits which are 0.
#define GPIO_CLR *(gpio + 10)	// Clears bits which are 1 and ignores bits which are 0.

#define BCM2708_PERI_BASE        0x20000000
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) // GPIO controller.

/*
 * All valid GPIOs found on the Raspberry Pi P1 Header.
 */
static const unsigned char all_valid_gpio[] = { 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 14, 15, 17, 18, 21, 22, 25, 27 };

/**
 * Set GPIO high.
 *
 * @param g_bit GPIO
 */
static void gpio_set(unsigned int g_bit) {
	GPIO_SET = g_bit;
}

/**
 * Set GPIO low.
 *
 * @param g_bit GPIO
 */
static void gpio_clear(unsigned int g_bit) {
	GPIO_CLR = g_bit;
}

/**
 * Set GPIO as input
 *
 * @param g_bit GPIO
 */
static void gpio_input(unsigned int g_bit) {
	INP_GPIO(g_bit);
}

/**
 * Set GPIO as output.
 *
 * @param g_bit GPIO
 */
static void gpio_output(unsigned int g_bit) {
	OUT_GPIO(g_bit);
}

/**
 * Activate internal pull-up.
 * 
 * @param g_bit GPIO
 */
static void gpio_enable_pull_up(unsigned int g_bit) {
	*(gpio + 37) = 2;
	udelay(10);
	*(gpio + 38) = g_bit;
	udelay(10);
	*(gpio + 37) = 0;
	*(gpio + 38) = 0;
}

/**
 * Read status of GPIO.
 *
 * @param g GPIO
 * @return Status of GPIO
 */
static unsigned char gpio_read(unsigned int g_bit) {
	return g_bit & *(gpio + 13);
}

/**
 * Read and negate status of all GPIOs.
 *
 * @return Negated status of all GPIOs
 */
static unsigned int gpio_read_all(void) {
	return ~(*(gpio + 13));;
}

/**
 * Init function for the gpio part of the driver.
 *
 * @return Result of the init operation
 */
static int __init gpio_init(void) {
	// Set up gpio pointer for direct register access.
	if ((gpio = ioremap(GPIO_BASE, 0xB0)) == NULL) {
		pr_err("io remap failed\n");
		return -EBUSY;
	}

	return 0;
}

/**
 * Exit function for the gpio part of the driver.
 */
static void gpio_exit(void) {
	iounmap(gpio);
}

/**
 * Check if a GPIO number is valid.
 * 
 * @param g_id GPIO number to test validness of
 * @return 1 if g is valid, otherwise 0
 */
static unsigned char gpio_valid(unsigned char g_id) {
	const int len = sizeof(all_valid_gpio) / sizeof(all_valid_gpio[0]);
	int i;

	for(i = 0; i < len; i++) {
		if(g_id == all_valid_gpio[i]) {
			return 1;
		}
	}
	return 0;
}


/**
 * Check if all GPIOs in the list are valid.
 * 
 * @param list List of GPIO id:s
 * @param len Length of list
 * @return 1 if all GPIOs in list is valid, otherwise 0
 */
static unsigned char gpio_list_valid(unsigned int *list, unsigned char len) {
	int i;
	// Check that all GPIO id:s are valid
	for(i = 0; i < len; i++) {
		if(!gpio_valid(list[i])) {
			return 0;
		}
	}
	return 1;
}

/**
 * Calculate the bit in the GPIO register that a specific GPIO number corresponds to.
 * 
 * @param g_id The GPIO number
 * @return The bit that GPIO g corresponds to in the GPIO register
 */
static unsigned int gpio_get_bit(unsigned char g_id) {
	return 1 << g_id;
}

/* _____          _     
  |  __ \        | |    
  | |__) |_ _  __| |___ 
  |  ___/ _` |/ _` / __|
  | |  | (_| | (_| \__ \
  |_|   \__,_|\__,_|___/
*/

#define DELAY 6
#define BUFFER_SIZE 34
#define BITS_LENGTH_MULTITAP 34
#define BITS_LENGTH 24
#define NUMBER_OF_GPIOS 6
#define NUMBER_OF_INPUT_DEVICES 5

/*
 * Structure that contain the configuration.
 *
 * Structuring of the gpio and gamepad arrays:
 * gpio: <clk, latch, port1_d0 (data1), port2_d0 (data2), port2_d1 (data4), port2_pp (data6)>
 * pad: <pad 1, pad 2, pad 3, pad 4, pad 5>
 *
 * multitap_enabled and fourscore_enabled are redable and writable from userspace (sysfs parameter).
 * There are no message to the driver when the variable are written. So they need to be handled as they can change at any time.
 *
 */
struct pads_config {
	unsigned int gpio[NUMBER_OF_GPIOS];
	struct input_dev *pad[NUMBER_OF_INPUT_DEVICES];
	unsigned char player_mode;
	char *device_name;
	int (* open) (struct input_dev *dev);
	void (* close) (struct input_dev *dev);
	bool multitap_enabled;
	bool fourscore_enabled;
};

// Buttons found on the SNES gamepad
static const long btn_label[] = { BTN_B, BTN_Y, BTN_SELECT, BTN_START, BTN_A, BTN_X, BTN_TL, BTN_TR };

// The order that the buttons of the SNES gamepad are stored in the byte string
static const unsigned char btn_index[] = { 0, 1, 2, 3, 8, 9, 10, 11 };

/**
 * Read the data pins of all connected devices.
 *
 * @param cfg The pad configuration
 * @param data Array to store the read data in
 */
static void pads_read(struct pads_config *cfg, unsigned int *data) {
	int i;
	unsigned int clk, latch;

	clk = cfg->gpio[0];
	latch = cfg->gpio[1];

	gpio_set(clk | latch);
	udelay(DELAY * 2);
	gpio_clear(latch);

	for (i = 0; i < BITS_LENGTH; i++) {
		udelay (DELAY);
		gpio_clear(clk);
		data[i] = gpio_read_all();
		udelay(DELAY);
		gpio_set(clk);
	}
}

/**
 * Read data pins of SNES Multitap and SNES pad connected to port 1.
 *
 * @param cfg The pad configuration
 * @param data Array to store the read data in
 */
static void pads_read_multitap(struct pads_config *cfg, unsigned int *data) {
	int i;
	unsigned int clk, latch, pp;

	clk = cfg->gpio[0];
	latch = cfg->gpio[1];
	pp = cfg->gpio[5];

	gpio_set(clk | latch);
	udelay(DELAY * 2);
	gpio_clear(latch);

	for (i = 0; i < BITS_LENGTH_MULTITAP / 2; i++) {
		udelay (DELAY);
		gpio_clear(clk);
		data[i] = gpio_read_all();
		udelay(DELAY);
		gpio_set(clk);
	}

	// Set PP low
	gpio_clear(pp);

	for (; i < BITS_LENGTH_MULTITAP; i++) {
		udelay (DELAY);
		gpio_clear(clk);
		data[i] = gpio_read_all();
		udelay(DELAY);
		gpio_set(clk);
	}

	// Set PP high
	gpio_set(pp);
}

/**
 * Check if a SNES Multitap is connected.
 *
 * @param cfg The pad configuration
 * @return 1 if a SNES Multitap is connected, otherwise 0
 */
static unsigned char multitap_connected(struct pads_config *cfg) {
	int i;
	unsigned char byte = 0;
	unsigned int clk, latch, d0, d1;

	// Store GPIOs in variables
	clk = cfg->gpio[0];
	latch = cfg->gpio[1];
	d0 = cfg->gpio[3];
	d1 = cfg->gpio[4];

	// Set D0 to output
	gpio_input(d0);
	gpio_output(d0);

	// Set D0 high
	gpio_set(d0);
	gpio_set(clk);
	udelay (DELAY);

	// Read D1 eight times
	for (i = 0; i < 8; i++) {
		udelay(DELAY);
		gpio_clear(clk);

		// Check if D1 is low
		if (!gpio_read(d1)) {
			return 0;
		}
		udelay(DELAY);
		gpio_set(clk);
	}

	// Set D0 low
	gpio_clear(d0);

	// Read D1 eight times
	for (i = 0; i < 8; i++) {
		udelay(DELAY);
		gpio_clear(clk);

		// Check if D1 is high
		if (gpio_read(d1)) {
			byte += 1;
		}
		byte <<= 1;
		udelay(DELAY);
		gpio_set(clk);
	}

	// Set D0 to input
	gpio_input(d0);

	if (byte == 0xFF) {
		return 0;
	}
	return 1;
}

/**
 * Check if a NES Four Score is connected.
 *
 * @param cfg The pad configuration
 * @return 1 if a NES Four Score is connected, otherwise 0
 */
static unsigned char fourscore_connected(struct pads_config *cfg, unsigned int *data) {
	return !(cfg->gpio[2] & data[16]) &&
	       !(cfg->gpio[2] & data[17]) &&
	       !(cfg->gpio[2] & data[18]) &&
	        (cfg->gpio[2] & data[19]) &&
	       !(cfg->gpio[2] & data[20]) &&
	       !(cfg->gpio[2] & data[21]) &&
	       !(cfg->gpio[2] & data[22]) &&
	       !(cfg->gpio[2] & data[23]) &&
	       !(cfg->gpio[3] & data[16]) &&
	       !(cfg->gpio[3] & data[17]) &&
	        (cfg->gpio[3] & data[18]) &&
	       !(cfg->gpio[3] & data[19]) &&
	       !(cfg->gpio[3] & data[20]) &&
	       !(cfg->gpio[3] & data[21]) &&
	       !(cfg->gpio[3] & data[22]) &&
	       !(cfg->gpio[3] & data[23]);
}

/**
 * Clear status of buttons and axises of pads not in use.
 * 
 * @param cfg The pad configuration
 * @param n_devs Number of devices to have all buttons and axises cleared
 */
static void pads_clear(struct pads_config *cfg, unsigned char n_devs) {
	struct input_dev *dev;
	int i, j;
	for(i = 0; i < n_devs; i++) {
		dev = cfg->pad[(NUMBER_OF_INPUT_DEVICES - 1) - i];
		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], 0);
		}
		input_report_abs(dev, ABS_X, 0);
		input_report_abs(dev, ABS_Y, 0);
		input_sync(dev);
	}
}

/**
 * Update the status of all connected devices.
 *
 * @param cfg The pad configuration
 */
static void pads_update(struct pads_config *cfg) {
	unsigned int g, data[BUFFER_SIZE];
	unsigned char i, j;
	struct input_dev *dev;

	if (cfg->multitap_enabled && multitap_connected(cfg)) {
		// SNES Multitap
		pads_read_multitap(cfg, data);
		
		// Set 5 player mode
		cfg->player_mode = 5;

		// Player 1
		dev = cfg->pad[0];
		g = cfg->gpio[2];

		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], g & data[btn_index[j]]);
		}
		input_report_abs(dev, ABS_X, !(g & data[6]) - !(g & data[7]));
		input_report_abs(dev, ABS_Y, !(g & data[4]) - !(g & data[5]));
		input_sync(dev);

		// Player 2
		dev = cfg->pad[1];
		g = cfg->gpio[3];

		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], g & data[btn_index[j]]);
		}
		input_report_abs(dev, ABS_X, !(g & data[6]) - !(g & data[7]));
		input_report_abs(dev, ABS_Y, !(g & data[4]) - !(g & data[5]));
		input_sync(dev);

		// Player 3
		dev = cfg->pad[2];
		g = cfg->gpio[4];

		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], g & data[btn_index[j]]);
		}
		input_report_abs(dev, ABS_X, !(g & data[6]) - !(g & data[7]));
		input_report_abs(dev, ABS_Y, !(g & data[4]) - !(g & data[5]));
		input_sync(dev);

		// Player 4
		dev = cfg->pad[3];
		g = cfg->gpio[3];

		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], g & data[btn_index[j] + 17]);
		}
		input_report_abs(dev, ABS_X, !(g & data[23]) - !(g & data[24]));
		input_report_abs(dev, ABS_Y, !(g & data[21]) - !(g & data[22]));
		input_sync(dev);

		// Player 5
		dev = cfg->pad[4];
		g = cfg->gpio[4];

		for (j = 0; j < 8; j++) {
			input_report_key(dev, btn_label[j], g & data[btn_index[j] + 17]);
		}
		input_report_abs(dev, ABS_X, !(g & data[23]) - !(g & data[24]));
		input_report_abs(dev, ABS_Y, !(g & data[21]) - !(g & data[22]));
		input_sync(dev);

	} else {
		pads_read(cfg, data);
	
		if (cfg->fourscore_enabled && fourscore_connected(cfg, data)) {
			// NES Four Score
	
			// Player 1 and 2
			for (i = 0; i < 2; i++) {
				dev = cfg->pad[i];
				g = cfg->gpio[i + 2];
	
				for (j = 0; j < 4; j++) {
					input_report_key(dev, btn_label[j], g & data[btn_index[j]]);
				}
				input_report_abs(dev, ABS_X, !(g & data[6]) - !(g & data[7]));
				input_report_abs(dev, ABS_Y, !(g & data[4]) - !(g & data[5]));
				input_sync(dev);
			}
	
			// Player 3 and 4
			for (i = 2; i < 4; i++) {
				dev = cfg->pad[i];
				g = cfg->gpio[i];
	
				for (j = 0; j < 4; j++) {
					input_report_key(dev, btn_label[j], g & data[btn_index[j] + 8]);
				}
				input_report_abs(dev, ABS_X, !(g & data[14]) - !(g & data[15]));
				input_report_abs(dev, ABS_Y, !(g & data[12]) - !(g & data[13]));
				input_sync(dev);
			}
			
			// Check if virtual device 5 should be cleared and if player_mode should be changed to 4 player mode
			if (cfg->player_mode > 4) {
				cfg->player_mode = 4;
				pads_clear(cfg, 1);
			} else if (cfg->player_mode < 4) {
				cfg->player_mode = 4;
			}
		} else {
			// NES or SNES gamepad
	
			// Player 1 and 2
			for (i = 0; i < 2; i++) {
				dev = cfg->pad[i];
				g = cfg->gpio[i + 2];
	
				for (j = 0; j < 8; j++) {
					input_report_key(dev, btn_label[j], g & data[btn_index[j]]);
				}
				input_report_abs(dev, ABS_X, !(g & data[6]) - !(g & data[7]));
				input_report_abs(dev, ABS_Y, !(g & data[4]) - !(g & data[5]));
				input_sync(dev);
			}
	
			// Check if virtual devices 3, 4 and 5 should be cleared and player_mode should be changed to 2 player mode
			if (cfg->player_mode > 2) {
				cfg->player_mode = 2;
				pads_clear(cfg, 3);
			}
		}
	}
}

/**
 * Setup all GPIOs.
 * 
 * @param cfg Pads config
 */
static void __init pads_setup_gpio(struct pads_config *cfg) {
	int i, bit;

	// Setup GPIO for clk and latch
	for(i = 0; i < 2; i++) {
		bit = cfg->gpio[i];
		gpio_output(bit);
	}
	
	// Setup GPIO for port1_d0, port2_d0, port2_d1
	for(i = 2; i < 5; i++) {
		bit = cfg->gpio[i];
		gpio_input(bit);
		gpio_enable_pull_up(bit);
	}
	
	// Setup GPIO for port1_pp
	bit = cfg->gpio[5];
	gpio_input(bit);
}

/**
 * Setup gamepads
 * 
 * @param cfg Pads configuration
 * @return Status
 */
static int __init pads_setup(struct pads_config *cfg) {
	int i, j;
	int status = 0;

	for (i = 0; (i < NUMBER_OF_INPUT_DEVICES) && (0 == status); ++i) {
		cfg->pad[i] = input_allocate_device();
		if (!cfg->pad[i]) {
			pr_err("Not enough memory for input device!\n");
			status = -ENOMEM;
		}

		if (status == 0) {
			// Allocate memory for the name
			char *phys = kzalloc(BUFFER_SIZE, GFP_KERNEL);
			if (!phys) {
				pr_err("Not enough memory for input device phys!\n");
				status = -ENOMEM;
			} else {
				// Create the device path name in userspace.
				snprintf(phys, BUFFER_SIZE, "input%d", i);
				cfg->pad[i]->phys = phys;
			}
		}

		if (status == 0) {
			// Configure the main part of the input device.
			cfg->pad[i]->name = cfg->device_name;
			cfg->pad[i]->id.bustype = BUS_PARPORT;
			cfg->pad[i]->id.vendor = 0x0001;
			cfg->pad[i]->id.product = 1;
			cfg->pad[i]->id.version = 0x0100;
    
			input_set_drvdata(cfg->pad[i], cfg);
    
			cfg->pad[i]->open = cfg->open;
			cfg->pad[i]->close = cfg->close;
			cfg->pad[i]->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
        
			for (j = 0; j < 2; j++) {
				input_set_abs_params(cfg->pad[i], ABS_X + j, -1, 1, 0, 0);
			}
            		
			for (j = 0; j < 8; j++) {
				__set_bit(btn_label[j], cfg->pad[i]->keybit);
			}
			
			status = input_register_device(cfg->pad[i]);
			if (status != 0) {
				pr_err("Could not register device no %i.\n", i);
				kfree(cfg->pad[i]->phys);
				input_free_device(cfg->pad[i]);
				cfg->pad[i] = NULL;
			}
		}
	}	

	if (status == 0) {
		// Done with the input event handlers. 
		// Setup the GPIO pins
		pads_setup_gpio(cfg);
	}
    
	return status;
}

static void __exit pads_remove(struct pads_config *cfg) {
	int idx;

	for (idx = 0; idx < NUMBER_OF_INPUT_DEVICES; idx++) {
		if (cfg->pad[idx]) {
			char *phys = (char*)cfg->pad[idx]->phys;
			input_unregister_device(cfg->pad[idx]);
			cfg->pad[idx] = NULL;
			kfree(phys);
		}
	}
}

/* _      _                     _                        _ 
  | |    (_)                   | |                      | |
  | |     _ _ __  _   ___  __  | | _____ _ __ _ __   ___| |
  | |    | | '_ \| | | \ \/ /  | |/ / _ \ '__| '_ \ / _ \ |
  | |____| | | | | |_| |>  <   |   <  __/ |  | | | |  __/ |
  |______|_|_| |_|\__,_/_/\_\  |_|\_\___|_|  |_| |_|\___|_|
*/

#define REFRESH_TIME HZ/100

MODULE_AUTHOR("Christian Isaksson");
MODULE_AUTHOR("Karl Thoren <karl.h.thoren@gmail.com>");
MODULE_DESCRIPTION("NES, SNES, gamepad driver for Raspberry Pi");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

/*
 * Structure that contain pads configuration, timer and mutex.
 */
struct snescon_config {
	struct pads_config pads_cfg;
	struct timer_list timer;
	struct mutex mutex;
	int driver_usage_cnt;
	unsigned int gpio_id[NUMBER_OF_GPIOS];
	unsigned int gpio_id_cnt; // Counter used in communication with userspace. Should be set to NUMBER_OF_GPIOS if parameter gpio_id is valid.
};

/**
 * Timer that read and update all pads.
 * 
 * @param ptr The pointer to the snescon_config structure
 */
static void snescon_timer(unsigned long ptr) {
	struct snescon_config* cfg = (void *) ptr;
	pads_update(&(cfg->pads_cfg));
	mod_timer(&cfg->timer, jiffies + REFRESH_TIME);
}

/**
 * @brief Open function for the driver.
 * Enables the 
 */
static int snescon_open(struct input_dev* dev) {
	struct snescon_config* cfg = input_get_drvdata(dev);
	int status;

	status = mutex_lock_interruptible(&cfg->mutex);
	if (status) {
		return status;
	}

	cfg->driver_usage_cnt++;
	if (cfg->driver_usage_cnt > 0) {
		// Atleast one device open. Start the timer or reset the timeout.
		mod_timer(&cfg->timer, jiffies + REFRESH_TIME);
	}

	mutex_unlock(&cfg->mutex);
	return 0;
}

/**
 * @brief Close function for the driver.
 * Disables the timer if the last device are closed.
 */
static void snescon_close(struct input_dev* dev) {
	struct snescon_config* cfg = input_get_drvdata(dev);

	mutex_lock(&cfg->mutex);
	cfg->driver_usage_cnt--;
	if (cfg->driver_usage_cnt <= 0) {
		// Last device closed. Disable the timer.
		del_timer_sync(&cfg->timer);
	}
	mutex_unlock(&cfg->mutex);
}

/**
 * Module global parameter variable.
 *
 */
static struct snescon_config snescon_config = {
	.gpio_id = {2, 3, 4, 7, 10, 11}, // Default values for the GPIOs.
	.gpio_id_cnt = NUMBER_OF_GPIOS,
	.pads_cfg.device_name = "SNES pad",
	.pads_cfg.open = &snescon_open,
	.pads_cfg.close = &snescon_close,
	.pads_cfg.multitap_enabled = 1,
	.pads_cfg.fourscore_enabled = 1,
};

/**
 * @brief Definition of module parameter gpio. This parameter are readable from the sysfs.
 */
module_param_array_named(gpio, snescon_config.gpio_id, uint, &(snescon_config.gpio_id_cnt), S_IRUGO);
MODULE_PARM_DESC(gpio, "Mapping of the 6 gpio for the driver are as follow: <clk, latch, port1_d0 (data1), port2_d0 (data2), port2_d1 (data4), port2_pp (data6)>");

/**
 * @brief Definition of module parameter multitap_enabled. This parameter are readable and writable from the sysfs.
 */
module_param_named(multitap, snescon_config.pads_cfg.multitap_enabled, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(multitap, "Enable/disable multitap. (Enabled by default.)");

/**
 * @brief Definition of module parameter fourscore_enabled. This parameter are readable and writable from the sysfs.
 */
module_param_named(fourscore, snescon_config.pads_cfg.fourscore_enabled, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(en_fourscore, "Enable/disable fourscore. (Enabled by default.)");

/**
 * Init function for the driver.
 */
static int __init snescon_init(void) {
	unsigned int i;
	unsigned int status = 0;
	
	// Check if the supplied GPIO setting are useful. All GPIOs must be set for the configuration to be prevalid.
	if (snescon_config.gpio_id_cnt != NUMBER_OF_GPIOS) {
		pr_err("Number of GPIO pins in gpio configuration is not correct. Expected %i, actual %i\n", NUMBER_OF_GPIOS, snescon_config.gpio_id_cnt);
		return -EINVAL;
	}

	// Final validation of the provided configuration.
	if (!gpio_list_valid(snescon_config.gpio_id, snescon_config.gpio_id_cnt)) {
		pr_err("One of the GPIO pins in the configuration are not valid!\n");
		return -EINVAL;
	}

	// Fill in the gpio struct with bit values.
	for (i = 0; i < NUMBER_OF_GPIOS; ++i) {
		snescon_config.pads_cfg.gpio[i] = gpio_get_bit(snescon_config.gpio_id[i]);
	}

	// Set up the gpio handler.
	if (gpio_init() != 0) {
		pr_err("Setup of the gpio handler failed\n");
		return -EBUSY;
	}

	status = pads_setup(&snescon_config.pads_cfg);
	if (status != 0) {
		pr_err("Setup of input_device failed!\n");

		// Cleanup allocated resourses
		gpio_exit();

		return status;
	}

	// Initiate the mutex and the timer
	mutex_init(&snescon_config.mutex);
	setup_timer(&snescon_config.timer, snescon_timer, (long) &snescon_config);
	
	pr_info("Loaded driver\n");

	return 0;
}

/**
 * Exit function for the driver.
 */
static void __exit snescon_exit(void) {
	del_timer(&snescon_config.timer);
	pads_remove(&snescon_config.pads_cfg);
	mutex_destroy(&snescon_config.mutex);
	gpio_exit();

	pr_info("driver exit\n");
}

module_init (snescon_init);
module_exit (snescon_exit);