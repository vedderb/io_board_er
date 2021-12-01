/*
	Copyright 2020 Benjamin Vedder	benjamin@vedder.se

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ch.h"
#include "hal.h"
#include "comm_can.h"
#include "adc_read.h"
#include "comm_usb.h"
#include "commands.h"
#include "terminal.h"
#include "main.h"

static PWMConfig pwmcfg = {
		21e6,
		1000,
		NULL,
		{
				{PWM_OUTPUT_ACTIVE_HIGH, NULL},
				{PWM_OUTPUT_DISABLED, NULL},
				{PWM_OUTPUT_DISABLED, NULL},
				{PWM_OUTPUT_DISABLED, NULL}
		},
		0,
		0
};

void main_set_digital_out(int ch, bool on) {
	switch (ch) {
		case 1:	if (on) OUT_ON(LINE_OUT_1); else OUT_OFF(LINE_OUT_1); break;
		case 2:	if (on) OUT_ON(LINE_OUT_2); else OUT_OFF(LINE_OUT_2); break;
		case 3:	if (on) OUT_ON(LINE_OUT_3); else OUT_OFF(LINE_OUT_3); break;
		case 4:	if (on) OUT_ON(LINE_OUT_4); else OUT_OFF(LINE_OUT_4); break;
		case 5:	if (on) OUT_ON(LINE_OUT_5); else OUT_OFF(LINE_OUT_5); break;
		case 6:	if (on) OUT_ON(LINE_OUT_6); else OUT_OFF(LINE_OUT_6); break;
		default: break;
	}
}

void main_set_pwm_out(int ch, float val) {
	switch (ch) {
	case 1: pwmEnableChannel(&PWMD4, 0, (uint32_t)(val * 1000.0)); break;
	default: break;
	}
}

int main(void) {
	halInit();
	chSysInit();

	palSetLineMode(LINE_LED_RED, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_LED_GREEN, PAL_MODE_OUTPUT_PUSHPULL);

	LED_OFF(LINE_LED_RED);
	LED_OFF(LINE_LED_GREEN);

	palSetLineMode(LINE_OUT_1, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_OUT_2, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_OUT_3, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_OUT_4, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_OUT_5, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLineMode(LINE_OUT_6, PAL_MODE_OUTPUT_PUSHPULL);

	OUT_OFF(LINE_OUT_1);
	OUT_OFF(LINE_OUT_2);
	OUT_OFF(LINE_OUT_3);
	OUT_OFF(LINE_OUT_4);
	OUT_OFF(LINE_OUT_5);
	OUT_OFF(LINE_OUT_6);

	palSetLineMode(LINE_LIM_SW_1, PAL_MODE_INPUT_PULLUP);
	palSetLineMode(LINE_LIM_SW_2, PAL_MODE_INPUT_PULLUP);
	palSetLineMode(LINE_LIM_SW_3, PAL_MODE_INPUT_PULLUP);
	palSetLineMode(LINE_LIM_SW_4, PAL_MODE_INPUT_PULLUP);

	comm_can_init();
	adc_read_init();
	comm_usb_init();
	commands_init();

	pwmStart(&PWMD4, &pwmcfg);
	main_set_pwm_out(1, 0.0);
	palSetPadMode(GPIOB, 6, PAL_MODE_ALTERNATE(2));

	for (;;) {
		LED_TOGGLE(LINE_LED_GREEN);
		chThdSleepMilliseconds(1000);
	}

	return 0;
}
