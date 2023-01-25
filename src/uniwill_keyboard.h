/*!
 * Copyright (c) 2020-2021 TUXEDO Computers GmbH <tux@tuxedocomputers.com>
 *
 * This file is part of tuxedo-keyboard.
 *
 * tuxedo-keyboard is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "tuxedo_keyboard_common.h"
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#include <linux/keyboard.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/string.h>
#include <linux/version.h>
#include "uniwill_interfaces.h"

#define UNIWILL_WMI_MGMT_GUID_BA "ABBC0F6D-8EA1-11D1-00A0-C90629100000"
#define UNIWILL_WMI_MGMT_GUID_BB "ABBC0F6E-8EA1-11D1-00A0-C90629100000"
#define UNIWILL_WMI_MGMT_GUID_BC "ABBC0F6F-8EA1-11D1-00A0-C90629100000"

#define UNIWILL_WMI_EVENT_GUID_0 "ABBC0F70-8EA1-11D1-00A0-C90629100000"
#define UNIWILL_WMI_EVENT_GUID_1 "ABBC0F71-8EA1-11D1-00A0-C90629100000"
#define UNIWILL_WMI_EVENT_GUID_2 "ABBC0F72-8EA1-11D1-00A0-C90629100000"

#define UNIWILL_OSD_RADIOON			0x01A
#define UNIWILL_OSD_RADIOOFF			0x01B
#define UNIWILL_OSD_KB_LED_LEVEL0		0x03B
#define UNIWILL_OSD_KB_LED_LEVEL1		0x03C
#define UNIWILL_OSD_KB_LED_LEVEL2		0x03D
#define UNIWILL_OSD_KB_LED_LEVEL3		0x03E
#define UNIWILL_OSD_KB_LED_LEVEL4		0x03F
#define UNIWILL_OSD_DC_ADAPTER_CHANGE		0x0AB

#define UNIWILL_KEY_RFKILL			0x0A4
#define UNIWILL_KEY_KBDILLUMDOWN		0x0B1
#define UNIWILL_KEY_KBDILLUMUP			0x0B2
#define UNIWILL_KEY_KBDILLUMTOGGLE		0x0B9

#define UNIWILL_OSD_TOUCHPADWORKAROUND		0xFFF

#define UNIWILL_BRIGHTNESS_MIN			0x00
#define UNIWILL_BRIGHTNESS_MAX			0xc8
#define UNIWILL_BRIGHTNESS_DEFAULT		UNIWILL_BRIGHTNESS_MAX * 0.30
#define UNIWILL_COLOR_DEFAULT			0xffffff

struct tuxedo_keyboard_driver uniwill_keyboard_driver;

struct kbd_led_state_uw_t {
	u32 brightness;
	u32 color;
} kbd_led_state_uw = {
	.brightness = UNIWILL_BRIGHTNESS_DEFAULT,
	.color = UNIWILL_COLOR_DEFAULT,
};

struct uniwill_device_features_t uniwill_device_features;

static u8 uniwill_kbd_bl_enable_state_on_start;
static bool uniwill_kbd_bl_type_rgb_single_color = true;

static struct key_entry uniwill_wmi_keymap[] = {
	// { KE_KEY,	UNIWILL_OSD_RADIOON,		{ KEY_RFKILL } },
	// { KE_KEY,	UNIWILL_OSD_RADIOOFF,		{ KEY_RFKILL } },
	// { KE_KEY,	0xb0,				{ KEY_F13 } },
	// Manual mode rfkill
	{ KE_KEY,	UNIWILL_KEY_RFKILL,		{ KEY_RFKILL }},
	{ KE_KEY,	UNIWILL_OSD_TOUCHPADWORKAROUND,	{ KEY_F21 } },
	// Keyboard brightness
	{ KE_KEY,	UNIWILL_KEY_KBDILLUMDOWN,	{ KEY_KBDILLUMDOWN } },
	{ KE_KEY,	UNIWILL_KEY_KBDILLUMUP,		{ KEY_KBDILLUMUP } },
	{ KE_KEY,	UNIWILL_KEY_KBDILLUMTOGGLE,	{ KEY_KBDILLUMTOGGLE } },
	// Only used to put ev bits
	{ KE_KEY,	0xffff,				{ KEY_F6 } },
	{ KE_KEY,	0xffff,				{ KEY_LEFTALT } },
	{ KE_KEY,	0xffff,				{ KEY_LEFTMETA } },
	{ KE_END,	0 }
};

static struct uniwill_interfaces_t {
	struct uniwill_interface_t *wmi;
} uniwill_interfaces = { .wmi = NULL };

uniwill_event_callb_t uniwill_event_callb;

u32 uniwill_read_ec_ram(u16 address, u8 *data)
{
	u32 status;

	if (!IS_ERR_OR_NULL(uniwill_interfaces.wmi))
		status = uniwill_interfaces.wmi->read_ec_ram(address, data);
	else {
		pr_err("no active interface while read addr 0x%04x\n", address);
		status = -EIO;
	}

	return status;
}
EXPORT_SYMBOL(uniwill_read_ec_ram);

u32 uniwill_write_ec_ram(u16 address, u8 data)
{
	u32 status;

	if (!IS_ERR_OR_NULL(uniwill_interfaces.wmi))
		status = uniwill_interfaces.wmi->write_ec_ram(address, data);
	else {
		pr_err("no active interface while write addr 0x%04x data 0x%02x\n", address, data);
		status = -EIO;
	}

	return status;
}
EXPORT_SYMBOL(uniwill_write_ec_ram);

u32 uniwill_write_ec_ram_with_retry(u16 address, u8 data, int retries)
{
	u32 status;
	int i;
	u8 control_data;

	for (i = 0; i < retries; ++i) {
		status = uniwill_write_ec_ram(address, data);
		if (status != 0) {
			msleep(50);
			continue;
		}
		else {
			status = uniwill_read_ec_ram(address, &control_data);
			if (status != 0 || data != control_data) {
				msleep(50);
				continue;
			}
			break;
		}
	}

	return status;
}
EXPORT_SYMBOL(uniwill_write_ec_ram_with_retry);

static DEFINE_MUTEX(uniwill_interface_modification_lock);

u32 uniwill_add_interface(struct uniwill_interface_t *interface)
{
	mutex_lock(&uniwill_interface_modification_lock);

	if (strcmp(interface->string_id, UNIWILL_INTERFACE_WMI_STRID) == 0)
		uniwill_interfaces.wmi = interface;
	else {
		TUXEDO_DEBUG("trying to add unknown interface\n");
		mutex_unlock(&uniwill_interface_modification_lock);
		return -EINVAL;
	}
	interface->event_callb = uniwill_event_callb;

	mutex_unlock(&uniwill_interface_modification_lock);

	// Initialize driver if not already present
	tuxedo_keyboard_init_driver(&uniwill_keyboard_driver);

	return 0;
}
EXPORT_SYMBOL(uniwill_add_interface);

u32 uniwill_remove_interface(struct uniwill_interface_t *interface)
{
	mutex_lock(&uniwill_interface_modification_lock);

	if (strcmp(interface->string_id, UNIWILL_INTERFACE_WMI_STRID) == 0) {
		// Remove driver if last interface is removed
		tuxedo_keyboard_remove_driver(&uniwill_keyboard_driver);

		uniwill_interfaces.wmi = NULL;
	} else {
		mutex_unlock(&uniwill_interface_modification_lock);
		return -EINVAL;
	}

	mutex_unlock(&uniwill_interface_modification_lock);

	return 0;
}
EXPORT_SYMBOL(uniwill_remove_interface);

u32 uniwill_get_active_interface_id(char **id_str)
{
	if (IS_ERR_OR_NULL(uniwill_interfaces.wmi))
		return -ENODEV;

	if (!IS_ERR_OR_NULL(id_str))
		*id_str = uniwill_interfaces.wmi->string_id;

	return 0;
}
EXPORT_SYMBOL(uniwill_get_active_interface_id);

static void key_event_work(struct work_struct *work)
{
	sparse_keymap_report_known_event(
		uniwill_keyboard_driver.input_device,
		UNIWILL_OSD_TOUCHPADWORKAROUND,
		1,
		true
	);
}

// Previous key codes for detecting longer combination
static u32 prev_key = 0, prevprev_key = 0;
static DECLARE_WORK(uniwill_key_event_work, key_event_work);

static int keyboard_notifier_callb(struct notifier_block *nb, unsigned long code, void *_param)
{
	struct keyboard_notifier_param *param = _param;
	int ret = NOTIFY_OK;

	if (!param->down) {

		if (code == KBD_KEYCODE) {
			switch (param->value) {
			case 125:
				// If the last keys up were 85 -> 29 -> 125
				// manually report KEY_F21
				if (prevprev_key == 85 && prev_key == 29) {
					TUXEDO_DEBUG("Touchpad Toggle\n");
					schedule_work(&uniwill_key_event_work);
					ret = NOTIFY_OK;
				}
				break;
			}
			prevprev_key = prev_key;
			prev_key = param->value;
		}
	}

	return ret;
}

static struct notifier_block keyboard_notifier_block = {
	.notifier_call = keyboard_notifier_callb
};

static u8 uniwill_read_kbd_bl_enabled(void)
{
	u8 backlight_data;
	u8 enabled = 0xff;

	uniwill_read_ec_ram(0x078c, &backlight_data);
	enabled = (backlight_data >> 1) & 0x01;
	enabled = !enabled;

	return enabled;
}

static void uniwill_write_kbd_bl_enable(u8 enable)
{
	u8 backlight_data;
	enable = enable & 0x01;

	uniwill_read_ec_ram(0x078c, &backlight_data);
	backlight_data = backlight_data & ~(1 << 1);
	backlight_data |= (!enable << 1);
	uniwill_write_ec_ram(0x078c, backlight_data);
}

/*static u32 uniwill_read_kbd_bl_br_state(u8 *brightness_state)
{
	u8 backlight_data;
	u32 result;

	uniwill_read_ec_ram(0x078c, &backlight_data);
	*brightness_state = (backlight_data & 0xf0) >> 4;
	result = 0;

	return result;
}*/

static u32 uniwill_read_kbd_bl_rgb(u8 *red, u8 *green, u8 *blue)
{
	u32 result;

	uniwill_read_ec_ram(0x1803, red);
	uniwill_read_ec_ram(0x1805, green);
	uniwill_read_ec_ram(0x1808, blue);

	result = 0;

	return result;
}

static void uniwill_write_kbd_bl_rgb(u8 red, u8 green, u8 blue)
{
	if (red > 0xc8) red = 0xc8;
	if (green > 0xc8) green = 0xc8;
	if (blue > 0xc8) blue = 0xc8;
	uniwill_write_ec_ram(0x1803, red);
	uniwill_write_ec_ram(0x1805, green);
	uniwill_write_ec_ram(0x1808, blue);
	TUXEDO_DEBUG("Wrote kbd color [%0#4x, %0#4x, %0#4x]\n", red, green, blue);
}

static void uniwill_write_kbd_bl_state(void) {
	// Get single colors from state
	u32 color_red = ((kbd_led_state_uw.color >> 0x10) & 0xff);
	u32 color_green = (kbd_led_state_uw.color >> 0x08) & 0xff;
	u32 color_blue = (kbd_led_state_uw.color >> 0x00) & 0xff;

	u32 brightness_percentage = (kbd_led_state_uw.brightness * 100) / UNIWILL_BRIGHTNESS_MAX;

	// Scale color values to valid range
	color_red = (color_red * 0xc8) / 0xff;
	color_green = (color_green * 0xc8) / 0xff;
	color_blue = (color_blue * 0xc8) / 0xff;

	// Scale the respective color values with brightness
	color_red = (color_red * brightness_percentage) / 100;
	color_green = (color_green * brightness_percentage) / 100;
	color_blue = (color_blue * brightness_percentage) / 100;

	uniwill_write_kbd_bl_rgb(color_red, color_green, color_blue);
}

static void uniwill_write_kbd_bl_reset(void)
{
	uniwill_write_ec_ram(0x078c, 0x10);
}

void uniwill_event_callb(u32 code)
{
	if (uniwill_keyboard_driver.input_device != NULL)
		if (!sparse_keymap_report_known_event(uniwill_keyboard_driver.input_device, code, 1, true)) {
			TUXEDO_DEBUG("Unknown code - %d (%0#6x)\n", code, code);
		}

	// Special key combination when mode change key is pressed
	if (code == 0xb0) {
		input_report_key(uniwill_keyboard_driver.input_device, KEY_LEFTMETA, 1);
		input_report_key(uniwill_keyboard_driver.input_device, KEY_LEFTALT, 1);
		input_report_key(uniwill_keyboard_driver.input_device, KEY_F6, 1);
		input_sync(uniwill_keyboard_driver.input_device);
		input_report_key(uniwill_keyboard_driver.input_device, KEY_F6, 0);
		input_report_key(uniwill_keyboard_driver.input_device, KEY_LEFTALT, 0);
		input_report_key(uniwill_keyboard_driver.input_device, KEY_LEFTMETA, 0);
		input_sync(uniwill_keyboard_driver.input_device);
	}

	// Keyboard backlight brightness toggle
	if (uniwill_kbd_bl_type_rgb_single_color) {
		switch (code) {
		case UNIWILL_OSD_KB_LED_LEVEL0:
			kbd_led_state_uw.brightness = 0x00;
			uniwill_write_kbd_bl_state();
			break;
		case UNIWILL_OSD_KB_LED_LEVEL1:
			kbd_led_state_uw.brightness = 0x20;
			uniwill_write_kbd_bl_state();
			break;
		case UNIWILL_OSD_KB_LED_LEVEL2:
			kbd_led_state_uw.brightness = 0x50;
			uniwill_write_kbd_bl_state();
			break;
		case UNIWILL_OSD_KB_LED_LEVEL3:
			kbd_led_state_uw.brightness = 0x80;
			uniwill_write_kbd_bl_state();
			break;
		case UNIWILL_OSD_KB_LED_LEVEL4:
			kbd_led_state_uw.brightness = 0xc8;
			uniwill_write_kbd_bl_state();
			break;
		// Also refresh keyboard state on cable switch event
		case UNIWILL_OSD_DC_ADAPTER_CHANGE:
			uniwill_write_kbd_bl_state();
			break;
		}
	}
}

static ssize_t uw_brightness_show(struct device *child,
				  struct device_attribute *attr, char *buffer)
{
	return sprintf(buffer, "%d\n", kbd_led_state_uw.brightness);
}

static ssize_t uw_brightness_store(struct device *child,
				   struct device_attribute *attr,
				   const char *buffer, size_t size)
{
	u32 brightness_input;
	int err = kstrtouint(buffer, 0, &brightness_input);
	if (err) return err;
	if (brightness_input > UNIWILL_BRIGHTNESS_MAX) return -EINVAL;
	kbd_led_state_uw.brightness = (u8)brightness_input;
	uniwill_write_kbd_bl_state();
	return size;
}

static ssize_t uw_color_string_show(struct device *child,
				 struct device_attribute *attr, char *buffer)
{
	int i;
	sprintf(buffer, "Color values:");
	for (i = 0; i < color_list.size; ++i) {
		sprintf(buffer + strlen(buffer), " %s",
			color_list.colors[i].name);
	}
	sprintf(buffer + strlen(buffer), "\n");
	return strlen(buffer);
}

static ssize_t uw_color_string_store(struct device *child,
				   struct device_attribute *attr,
				   const char *buffer, size_t size)
{
	u32 color_value;
	char *buffer_copy;
	
	buffer_copy = kmalloc(size + 1, GFP_KERNEL);
	strcpy(buffer_copy, buffer);
	color_value = color_lookup(&color_list, strstrip(buffer_copy));
	kfree(buffer_copy);

	if (color_value > 0xffffff) return -EINVAL;
	kbd_led_state_uw.color = color_value;
	uniwill_write_kbd_bl_state();
	return size;
}

// Device attributes used by uw kbd
struct uw_kbd_dev_attrs_t {
	struct device_attribute brightness;
	struct device_attribute color_string;
} uw_kbd_dev_attrs = {
	.brightness = __ATTR(brightness, 0644, uw_brightness_show, uw_brightness_store),
	.color_string = __ATTR(color_string, 0644, uw_color_string_show, uw_color_string_store)
};

// Device attributes used for uw_kbd_bl_color
static struct attribute *uw_kbd_bl_color_attrs[] = {
	&uw_kbd_dev_attrs.brightness.attr,
	&uw_kbd_dev_attrs.color_string.attr,
	NULL
};

static struct attribute_group uw_kbd_bl_color_attr_group = {
	.name = "uw_kbd_bl_color",
	.attrs = uw_kbd_bl_color_attrs
};

static void uw_kbd_bl_init_set(void)
{
	if (uniwill_kbd_bl_type_rgb_single_color) {
		// Reset keyboard backlight
		uniwill_write_kbd_bl_reset();
		// Make sure reset finish before continue
		msleep(100);

		// Disable backlight while initializing
		// uniwill_write_kbd_bl_enable(0);

		// Update keyboard backlight according to the current state
		uniwill_write_kbd_bl_state();
	}

	// Enable keyboard backlight
	uniwill_write_kbd_bl_enable(1);
}

// Keep track of previous colors on start, init array with different non-colors
static u32 uw_prev_colors[] = {0x01000000, 0x02000000, 0x03000000};
static u32 uw_prev_colors_size = 3;
static u32 uw_prev_colors_index = 0;

// Timer for checking animation colors
static struct timer_list uw_kbd_bl_init_timer;
static volatile int uw_kbd_bl_check_count = 40;
static int uw_kbd_bl_init_check_interval_ms = 500;

static void uw_kbd_bl_init_ready_check_work_func(struct work_struct *work)
{
	u8 uw_cur_red, uw_cur_green, uw_cur_blue;
	int i;
	bool prev_colors_same;
	uniwill_read_kbd_bl_rgb(&uw_cur_red, &uw_cur_green, &uw_cur_blue);
	uw_prev_colors[uw_prev_colors_index] = (uw_cur_red << 0x10) | (uw_cur_green << 0x08) | uw_cur_blue;
	uw_prev_colors_index = (uw_prev_colors_index + 1) % uw_prev_colors_size;

	prev_colors_same = true;
	for (i = 1; i < uw_prev_colors_size; ++i) {
		if (uw_prev_colors[i-1] != uw_prev_colors[i]) prev_colors_same = false;
	}

	if (prev_colors_same) {
		uw_kbd_bl_init_set();
		del_timer(&uw_kbd_bl_init_timer);
	} else {
		if (uw_kbd_bl_check_count != 0) {
			mod_timer(&uw_kbd_bl_init_timer, jiffies + msecs_to_jiffies(uw_kbd_bl_init_check_interval_ms));
		} else {
			TUXEDO_INFO("uw kbd init timeout, failed to detect end of boot animation\n");
			del_timer(&uw_kbd_bl_init_timer);
		}
	}

	uw_kbd_bl_check_count -= 1;
}

static DECLARE_WORK(uw_kbd_bl_init_ready_check_work, uw_kbd_bl_init_ready_check_work_func);

static void uw_kbd_bl_init_ready_check(struct timer_list *t)
{
	schedule_work(&uw_kbd_bl_init_ready_check_work);
}

static int uw_kbd_bl_init(struct platform_device *dev)
{
	int status = 0;

	uniwill_kbd_bl_type_rgb_single_color = false
		// New names
		| dmi_match(DMI_BOARD_NAME, "POLARIS1501A1650TI")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1501A2060")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1501I1650TI")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1501I2060")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1701A1650TI")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1701A2060")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1701I1650TI")
		| dmi_match(DMI_BOARD_NAME, "POLARIS1701I2060")
		| dmi_match(DMI_BOARD_NAME, "GMxNGxx")
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
		| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XA02")
		| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XI02")
		| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XA03")
		| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XI03")
#endif

		// Old names
		// | dmi_match(DMI_BOARD_NAME, "Polaris15I01")
		// | dmi_match(DMI_BOARD_NAME, "Polaris17I01")
		// | dmi_match(DMI_BOARD_NAME, "Polaris15A01")
		// | dmi_match(DMI_BOARD_NAME, "Polaris1501I2060")
		// | dmi_match(DMI_BOARD_NAME, "Polaris1701I2060")
		;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
	TUXEDO_ERROR(
		"Warning: Kernel version less that 4.18, keyboard backlight might not be properly recognized.");
#endif

	// Save previous enable state
	uniwill_kbd_bl_enable_state_on_start = uniwill_read_kbd_bl_enabled();

	if (uniwill_kbd_bl_type_rgb_single_color) {
		// Initialize keyboard backlight driver state according to parameters
		if (param_brightness > UNIWILL_BRIGHTNESS_MAX) param_brightness = UNIWILL_BRIGHTNESS_DEFAULT;
		kbd_led_state_uw.brightness = param_brightness;
		if (color_lookup(&color_list, param_color) <= (u32) 0xffffff) kbd_led_state_uw.color = color_lookup(&color_list, param_color);
		else kbd_led_state_uw.color = UNIWILL_COLOR_DEFAULT;

		// Init sysfs bl attributes group
		status = sysfs_create_group(&dev->dev.kobj, &uw_kbd_bl_color_attr_group);
		if (status) TUXEDO_ERROR("Failed to create sysfs group\n");

		// Start periodic checking of animation, set and enable bl when done
		timer_setup(&uw_kbd_bl_init_timer, uw_kbd_bl_init_ready_check, 0);
		mod_timer(&uw_kbd_bl_init_timer, jiffies + msecs_to_jiffies(uw_kbd_bl_init_check_interval_ms));
	} else {
		// For non-RGB versions
		// Enable keyboard backlight immediately (should it be disabled)
		uniwill_write_kbd_bl_enable(1);
	}

	return status;
}

#define UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS	0x24
#define UNIWILL_LIGHTBAR_LED_NAME_RGB_RED	"lightbar_rgb:1:status"
#define UNIWILL_LIGHTBAR_LED_NAME_RGB_GREEN	"lightbar_rgb:2:status"
#define UNIWILL_LIGHTBAR_LED_NAME_RGB_BLUE	"lightbar_rgb:3:status"
#define UNIWILL_LIGHTBAR_LED_NAME_ANIMATION	"lightbar_animation::status"

static void uniwill_write_lightbar_rgb(u8 red, u8 green, u8 blue)
{
	if (red <= UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS) {
		uniwill_write_ec_ram(0x0749, red);
	}
	if (green <= UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS) {
		uniwill_write_ec_ram(0x074a, green);
	}
	if (blue <= UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS) {
		uniwill_write_ec_ram(0x074b, blue);
	}
}

static void uniwill_read_lightbar_rgb(u8 *red, u8 *green, u8 *blue)
{
	uniwill_read_ec_ram(0x0749, red);
	uniwill_read_ec_ram(0x074a, green);
	uniwill_read_ec_ram(0x074b, blue);
}

static void uniwill_write_lightbar_animation(bool animation_status)
{
	u8 value;

	uniwill_read_ec_ram(0x0748, &value);
	if (animation_status) {
		value |= 0x80;
	} else {
		value &= ~0x80;
	}
	uniwill_write_ec_ram(0x0748, value);
}

static void uniwill_read_lightbar_animation(bool *animation_status)
{
	u8 lightbar_animation_data;
	uniwill_read_ec_ram(0x0748, &lightbar_animation_data);
	*animation_status = (lightbar_animation_data & 0x80) > 0;
}

static int lightbar_set_blocking(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	u8 red = 0xff, green = 0xff, blue = 0xff;
	bool led_red = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_RED) != NULL;
	bool led_green = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_GREEN) != NULL;
	bool led_blue = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_BLUE) != NULL;
	bool led_animation = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_ANIMATION) != NULL;

	if (led_red || led_green || led_blue) {
		if (led_red) {
			red = brightness;
		} else if (led_green) {
			green = brightness;
		} else if (led_blue) {
			blue = brightness;
		}
		uniwill_write_lightbar_rgb(red, green, blue);
		// Also make sure the animation is off
		uniwill_write_lightbar_animation(false);
	} else if (led_animation) {
		if (brightness == 1) {
			uniwill_write_lightbar_animation(true);
		} else {
			uniwill_write_lightbar_animation(false);
		}
	}
	return 0;
}

static enum led_brightness lightbar_get(struct led_classdev *led_cdev)
{
	u8 red, green, blue;
	bool animation_status;
	bool led_red = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_RED) != NULL;
	bool led_green = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_GREEN) != NULL;
	bool led_blue = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_RGB_BLUE) != NULL;
	bool led_animation = strstr(led_cdev->name, UNIWILL_LIGHTBAR_LED_NAME_ANIMATION) != NULL;

	if (led_red || led_green || led_blue) {
		uniwill_read_lightbar_rgb(&red, &green, &blue);
		if (led_red) {
			return red;
		} else if (led_green) {
			return green;
		} else if (led_blue) {
			return blue;
		}
	} else if (led_animation) {
		uniwill_read_lightbar_animation(&animation_status);
		return animation_status ? 1 : 0;
	}

	return 0;
}

static bool uw_lightbar_loaded;
static struct led_classdev lightbar_led_classdevs[] = {
	{
		.name = UNIWILL_LIGHTBAR_LED_NAME_RGB_RED,
		.max_brightness = UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS,
		.brightness_set_blocking = &lightbar_set_blocking,
		.brightness_get = &lightbar_get
	},
	{
		.name = UNIWILL_LIGHTBAR_LED_NAME_RGB_GREEN,
		.max_brightness = UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS,
		.brightness_set_blocking = &lightbar_set_blocking,
		.brightness_get = &lightbar_get
	},
	{
		.name = UNIWILL_LIGHTBAR_LED_NAME_RGB_BLUE,
		.max_brightness = UNIWILL_LIGHTBAR_LED_MAX_BRIGHTNESS,
		.brightness_set_blocking = &lightbar_set_blocking,
		.brightness_get = &lightbar_get
	},
	{
		.name = UNIWILL_LIGHTBAR_LED_NAME_ANIMATION,
		.max_brightness = 1,
		.brightness_set_blocking = &lightbar_set_blocking,
		.brightness_get = &lightbar_get
	}
};

static int uw_lightbar_init(struct platform_device *dev)
{
	int i, j, status;

	bool lightbar_supported = false
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71A")
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71B")
		|| dmi_match(DMI_BOARD_NAME, "TRINITY1501I")
		|| dmi_match(DMI_BOARD_NAME, "TRINITY1701I")
		|| dmi_match(DMI_PRODUCT_NAME, "A60 MUV")
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XI03")
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XA03")
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XI04")
		|| dmi_match(DMI_PRODUCT_SKU, "STEPOL1XA04")

#endif
		;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
	TUXEDO_ERROR(
		"Warning: Kernel version less that 4.18, lightbar might not be properly recognized.");
#endif

	if (!lightbar_supported)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(lightbar_led_classdevs); ++i) {
		status = led_classdev_register(&dev->dev, &lightbar_led_classdevs[i]);
		if (status < 0) {
			for (j = 0; j < i; j++)
				led_classdev_unregister(&lightbar_led_classdevs[j]);
			return status;
		}
	}

	// Init default state
	uniwill_write_lightbar_animation(false);
	uniwill_write_lightbar_rgb(0, 0, 0);

	return 0;
}

static int uw_lightbar_remove(struct platform_device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(lightbar_led_classdevs); ++i) {
		led_classdev_unregister(&lightbar_led_classdevs[i]);
	}
	return 0;
}

static bool uw_charging_prio_loaded = false;

/*
 * charging_prio values
 *     0 => charging priority
 *     1 => performance priority
 */
static int uw_set_charging_priority(u8 charging_priority)
{
	u8 previous_data, next_data;
	int result;

	charging_priority = (charging_priority & 0x01) << 7;

	result = uniwill_read_ec_ram(0x07cc, &previous_data);
	if (result != 0)
		return result;

	next_data = (previous_data & ~(1 << 7)) | charging_priority;
	result = uniwill_write_ec_ram(0x07cc, next_data);

	return result;
}

static int uw_get_charging_priority(u8 *charging_priority)
{
	int result = uniwill_read_ec_ram(0x07cc, charging_priority);
	*charging_priority = (*charging_priority >> 7) & 0x01;
	return result;
}

static int uw_has_charging_priority(bool *status)
{
	u8 data;
	int result;

	bool not_supported_device = false
		|| dmi_match(DMI_BOARD_NAME, "PF5PU1G")
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71A")
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71B")
		|| dmi_match(DMI_PRODUCT_NAME, "A60 MUV")
	;

	if (not_supported_device)
		return false;

	result = uniwill_read_ec_ram(0x0742, &data);

	if (data & (1 << 5))
		*status = true;
	else
		*status = false;

	return result;
}

static bool uw_charging_profile_loaded = false;

/*
 * charging_profile values
 *     0 => high capacity
 *     1 => balanced
 *     2 => stationary
 */
static int uw_set_charging_profile(u8 charging_profile)
{
	u8 previous_data, next_data;
	int result;

	charging_profile = (charging_profile & 0x03) << 4;

	result = uniwill_read_ec_ram(0x07a6, &previous_data);
	if (result != 0)
		return result;

	next_data = (previous_data & ~(0x03 << 4)) | charging_profile;
	result = uniwill_write_ec_ram(0x07a6, next_data);

	return result;
}

static int uw_get_charging_profile(u8 *charging_profile)
{
	int result = uniwill_read_ec_ram(0x07a6, charging_profile);
	*charging_profile = (*charging_profile >> 4) & 0x03;
	return result;
}

static int uw_has_charging_profile(bool *status)
{
	u8 data;
	int result;

	bool not_supported_device = false
		|| dmi_match(DMI_BOARD_NAME, "PF5PU1G")
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71A")
		|| dmi_match(DMI_BOARD_NAME, "LAPQC71B")
		|| dmi_match(DMI_PRODUCT_NAME, "A60 MUV")
	;

	if (not_supported_device)
		return false;

	result = uniwill_read_ec_ram(0x078e, &data);

	if (data & (1 << 3))
		*status = true;
	else
		*status = false;

	return result;
}

struct char_to_u8_t {
	char* descriptor;
	u8 value;
};

static struct char_to_u8_t charging_profile_options[] = {
	{ .descriptor = "high_capacity", .value = 0x00 },
	{ .descriptor = "balanced",	 .value = 0x01 },
	{ .descriptor = "stationary",	 .value = 0x02 }
};

static ssize_t uw_charging_profiles_available_show(struct device *child,
						   struct device_attribute *attr,
						   char *buffer)
{
	int i, n;
	n = ARRAY_SIZE(charging_profile_options);
	for (i = 0; i < n; ++i) {
		sprintf(buffer + strlen(buffer), "%s",
			charging_profile_options[i].descriptor);
		if (i < n - 1)
			sprintf(buffer + strlen(buffer), " ");
		else
			sprintf(buffer + strlen(buffer), "\n");
	}

	return strlen(buffer);
}

static ssize_t uw_charging_profile_show(struct device *child,
					struct device_attribute *attr, char *buffer)
{
	u8 charging_profile_value;
	int i, result;

	result = uw_get_charging_profile(&charging_profile_value);
	if (result != 0)
		return result;

	for (i = 0; i < ARRAY_SIZE(charging_profile_options); ++i)
		if (charging_profile_options[i].value == charging_profile_value) {
			sprintf(buffer, "%s\n", charging_profile_options[i].descriptor);
			return strlen(buffer);
		}

	pr_err("Read charging profile value not matched to a descriptor\n");

	return -EIO;
}

static ssize_t uw_charging_profile_store(struct device *child,
					 struct device_attribute *attr,
					 const char *buffer, size_t size)
{
	u8 charging_profile_value;
	int i, result;
	char *buffer_copy;
	char *charging_profile_descriptor;
	buffer_copy = kmalloc(size + 1, GFP_KERNEL);
	strcpy(buffer_copy, buffer);
	charging_profile_descriptor = strstrip(buffer_copy);

	for (i = 0; i < ARRAY_SIZE(charging_profile_options); ++i)
		if (strcmp(charging_profile_options[i].descriptor, charging_profile_descriptor) == 0) {
			charging_profile_value = charging_profile_options[i].value;
			break;
		}

	kfree(buffer_copy);

	if (i < ARRAY_SIZE(charging_profile_options)) {
		// Option found try to set
		result = uw_set_charging_profile(charging_profile_value);
		if (result == 0)
			return size;
		else
			return -EIO;
	} else
		// Invalid input, not matched to an option
		return -EINVAL;
}

struct uw_charging_profile_attrs_t {
	struct device_attribute charging_profiles_available;
	struct device_attribute charging_profile;
} uw_charging_profile_attrs = {
	.charging_profiles_available = __ATTR(charging_profiles_available, 0444, uw_charging_profiles_available_show, NULL),
	.charging_profile = __ATTR(charging_profile, 0644, uw_charging_profile_show, uw_charging_profile_store)
};

static struct attribute *uw_charging_profile_attrs_list[] = {
	&uw_charging_profile_attrs.charging_profiles_available.attr,
	&uw_charging_profile_attrs.charging_profile.attr,
	NULL
};

static struct attribute_group uw_charging_profile_attr_group = {
	.name = "charging_profile",
	.attrs = uw_charging_profile_attrs_list
};

static struct char_to_u8_t charging_prio_options[] = {
	{ .descriptor = "charge_battery", .value = 0x00 },
	{ .descriptor = "performance",    .value = 0x01 }
};

static ssize_t uw_charging_prios_available_show(struct device *child,
						struct device_attribute *attr,
						char *buffer)
{
	int i, n;
	n = ARRAY_SIZE(charging_prio_options);
	for (i = 0; i < n; ++i) {
		sprintf(buffer + strlen(buffer), "%s",
			charging_prio_options[i].descriptor);
		if (i < n - 1)
			sprintf(buffer + strlen(buffer), " ");
		else
			sprintf(buffer + strlen(buffer), "\n");
	}

	return strlen(buffer);
}

static ssize_t uw_charging_prio_show(struct device *child,
				     struct device_attribute *attr, char *buffer)
{
	u8 charging_prio_value;
	int i, result;

	result = uw_get_charging_priority(&charging_prio_value);
	if (result != 0)
		return result;

	for (i = 0; i < ARRAY_SIZE(charging_prio_options); ++i)
		if (charging_prio_options[i].value == charging_prio_value) {
			sprintf(buffer, "%s\n", charging_prio_options[i].descriptor);
			return strlen(buffer);
		}

	pr_err("Read charging prio value not matched to a descriptor\n");

	return -EIO;
}

static ssize_t uw_charging_prio_store(struct device *child,
				      struct device_attribute *attr,
				      const char *buffer, size_t size)
{
	u8 charging_prio_value;
	int i, result;
	char *buffer_copy;
	char *charging_prio_descriptor;
	buffer_copy = kmalloc(size + 1, GFP_KERNEL);
	strcpy(buffer_copy, buffer);
	charging_prio_descriptor = strstrip(buffer_copy);

	for (i = 0; i < ARRAY_SIZE(charging_prio_options); ++i)
		if (strcmp(charging_prio_options[i].descriptor, charging_prio_descriptor) == 0) {
			charging_prio_value = charging_prio_options[i].value;
			break;
		}

	kfree(buffer_copy);

	if (i < ARRAY_SIZE(charging_prio_options)) {
		// Option found try to set
		result = uw_set_charging_priority(charging_prio_value);
		if (result == 0)
			return size;
		else
			return -EIO;
	} else
		// Invalid input, not matched to an option
		return -EINVAL;
}

struct uw_charging_prio_attrs_t {
	struct device_attribute charging_prios_available;
	struct device_attribute charging_prio;
} uw_charging_prio_attrs = {
	.charging_prios_available = __ATTR(charging_prios_available, 0444, uw_charging_prios_available_show, NULL),
	.charging_prio = __ATTR(charging_prio, 0644, uw_charging_prio_show, uw_charging_prio_store)
};

static struct attribute *uw_charging_prio_attrs_list[] = {
	&uw_charging_prio_attrs.charging_prios_available.attr,
	&uw_charging_prio_attrs.charging_prio.attr,
	NULL
};

static struct attribute_group uw_charging_prio_attr_group = {
	.name = "charging_priority",
	.attrs = uw_charging_prio_attrs_list
};

struct uniwill_device_features_t *uniwill_get_device_features(void)
{
	struct uniwill_device_features_t *uw_feats = &uniwill_device_features;
	u32 status;

	status = uniwill_read_ec_ram(0x0740, &uw_feats->model);
	if (status != 0)
		uw_feats->model = 0;

	uw_feats->uniwill_profile_v1_two_profs = false
		|| dmi_match(DMI_BOARD_NAME, "PF5PU1G")
		|| dmi_match(DMI_BOARD_NAME, "PULSE1401")
		|| dmi_match(DMI_BOARD_NAME, "PULSE1501")
	;

	uw_feats->uniwill_profile_v1_three_profs = false
	// Devices with "classic" profile support
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1501A1650TI")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1501A2060")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1501I1650TI")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1501I2060")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1701A1650TI")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1701A2060")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1701I1650TI")
		|| dmi_match(DMI_BOARD_NAME, "POLARIS1701I2060")
		// Note: XMG Fusion removed for now, seem to have
		// neither same power profile control nor TDP set
		//|| dmi_match(DMI_BOARD_NAME, "LAPQC71A")
		//|| dmi_match(DMI_BOARD_NAME, "LAPQC71B")
		//|| dmi_match(DMI_PRODUCT_NAME, "A60 MUV")
	;

	uw_feats->uniwill_profile_v1_three_profs_leds_only = false
	// Devices where profile mainly controls power profile LED status
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
		|| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XA02")
		|| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XI02")
		|| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XA03")
		|| dmi_match(DMI_PRODUCT_SKU, "POLARIS1XI03")
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XI03")
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XA03")
		|| dmi_match(DMI_PRODUCT_SKU, "STELLARIS1XI04")
		|| dmi_match(DMI_PRODUCT_SKU, "STEPOL1XA04")
#endif
	;

	uw_feats->uniwill_profile_v1 =
		uw_feats->uniwill_profile_v1_two_profs ||
		uw_feats->uniwill_profile_v1_three_profs;

	uw_has_charging_priority(&uw_feats->uniwill_has_charging_prio);
	uw_has_charging_profile(&uw_feats->uniwill_has_charging_profile);

	return uw_feats;
}
EXPORT_SYMBOL(uniwill_get_device_features);

static int uniwill_keyboard_probe(struct platform_device *dev)
{
	u32 i;
	u8 data;
	int status;

	struct uniwill_device_features_t *uw_feats = uniwill_get_device_features();

	// FIXME Hard set balanced profile until we have implemented a way to
	// switch it while tuxedo_io is loaded
	// uw_ec_write_addr(0x51, 0x07, 0x00, 0x00, &reg_write_return);
	uniwill_write_ec_ram(0x0751, 0x00);

	if (uw_feats->uniwill_profile_v1) {
		// Set manual-mode fan-curve in 0x0743 - 0x0747
		// Some kind of default fan-curve is stored in 0x0786 - 0x078a: Using it to initialize manual-mode fan-curve
		for (i = 0; i < 5; ++i) {
			uniwill_read_ec_ram(0x0786 + i, &data);
			uniwill_write_ec_ram(0x0743 + i, data);
		}
	}
	else {
		// Activate NVIDIA Dynamic Boost
		uniwill_write_ec_ram(0x0746, 0x19);
		uniwill_write_ec_ram(0x0745, 0x23);
		uniwill_write_ec_ram(0x0743, 0x03);
	}

	// Enable manual mode
	uniwill_write_ec_ram(0x0741, 0x01);

	// Zero second fan temp for detection
	uniwill_write_ec_ram(0x044f, 0x00);

	status = register_keyboard_notifier(&keyboard_notifier_block);

	uw_kbd_bl_init(dev);

	status = uw_lightbar_init(dev);
	uw_lightbar_loaded = (status >= 0);

	if (uw_feats->uniwill_has_charging_prio)
		uw_charging_prio_loaded = sysfs_create_group(&dev->dev.kobj, &uw_charging_prio_attr_group) == 0;

	if (uw_feats->uniwill_has_charging_profile)
		uw_charging_profile_loaded = sysfs_create_group(&dev->dev.kobj, &uw_charging_profile_attr_group) == 0;

	return 0;
}

static int uniwill_keyboard_remove(struct platform_device *dev)
{
	if (uw_charging_prio_loaded)
		sysfs_remove_group(&dev->dev.kobj, &uw_charging_prio_attr_group);

	if (uw_charging_profile_loaded)
		sysfs_remove_group(&dev->dev.kobj, &uw_charging_profile_attr_group);

	if (uniwill_kbd_bl_type_rgb_single_color) {
		sysfs_remove_group(&dev->dev.kobj, &uw_kbd_bl_color_attr_group);
	}

	// Restore previous backlight enable state
	if (uniwill_kbd_bl_enable_state_on_start != 0xff) {
		uniwill_write_kbd_bl_enable(uniwill_kbd_bl_enable_state_on_start);
	}

	unregister_keyboard_notifier(&keyboard_notifier_block);

	del_timer(&uw_kbd_bl_init_timer);

	if (uw_lightbar_loaded)
		uw_lightbar_remove(dev);

	// Disable manual mode
	uniwill_write_ec_ram(0x0741, 0x00);

	return 0;
}

static int uniwill_keyboard_suspend(struct platform_device *dev, pm_message_t state)
{
	uniwill_write_kbd_bl_enable(0);
	return 0;
}

static int uniwill_keyboard_resume(struct platform_device *dev)
{
	if (uniwill_kbd_bl_type_rgb_single_color) {
		uniwill_write_kbd_bl_reset();
		msleep(100);
		uniwill_write_kbd_bl_state();
	}
	uniwill_write_kbd_bl_enable(1);
	return 0;
}

static struct platform_driver platform_driver_uniwill = {
	.remove = uniwill_keyboard_remove,
	.suspend = uniwill_keyboard_suspend,
	.resume = uniwill_keyboard_resume,
	.driver =
		{
			.name = DRIVER_NAME,
			.owner = THIS_MODULE,
		},
};

struct tuxedo_keyboard_driver uniwill_keyboard_driver = {
	.platform_driver = &platform_driver_uniwill,
	.probe = uniwill_keyboard_probe,
	.key_map = uniwill_wmi_keymap,
};
