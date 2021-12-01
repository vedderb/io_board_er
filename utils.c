#include "utils.h"

/**
 * Get the middle value of three values
 *
 * @param a
 * First value
 *
 * @param b
 * Second value
 *
 * @param c
 * Third value
 *
 * @return
 * The middle value
 */
int utils_middle_of_3_int(int a, int b, int c) {
	int middle;

	if ((a <= b) && (a <= c)) {
		middle = (b <= c) ? b : c;
	} else if ((b <= a) && (b <= c)) {
		middle = (a <= c) ? a : c;
	} else {
		middle = (a <= b) ? a : b;
	}
	return middle;
}

uint32_t utils_crc32c(uint8_t *data, uint32_t len) {
	uint32_t crc = 0xFFFFFFFF;

	for (uint32_t i = 0; i < len;i++) {
		uint32_t byte = data[i];
		crc = crc ^ byte;

		for (int j = 7;j >= 0;j--) {
			uint32_t mask = -(crc & 1);
			crc = (crc >> 1) ^ (0x82F63B78 & mask);
		}
	}

	return ~crc;
}

const char* utils_fault_to_string(sw_fault_code fault) {
	switch (fault) {
	case FAULT_CODE_NONE: return "FAULT_CODE_NONE"; break;
	default: return "FAULT_UNKNOWN"; break;
	}
}

const char* utils_hw_type_to_string(HW_TYPE hw) {
	switch (hw) {
	case HW_TYPE_VESC: return "HW_TYPE_VESC"; break;
	case HW_TYPE_VESC_BMS: return "HW_TYPE_VESC_BMS"; break;
	case HW_TYPE_CUSTOM_MODULE: return "HW_TYPE_CUSTOM_MODULE"; break;
	default: return "FAULT_HARDWARE"; break;
	}
}
