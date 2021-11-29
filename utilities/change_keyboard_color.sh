#!/bin/bash
set_color() {
echo $1 > /sys/devices/platform/tuxedo_keyboard/uw_kbd_bl_color/color_string;
}

if ! set_color $1; then
	echo "Not allowed."
	cat /sys/devices/platform/tuxedo_keyboard/uw_kbd_bl_color/color_string;
fi
