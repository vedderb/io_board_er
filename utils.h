#ifndef UTILS_H_
#define UTILS_H_

#include "datatypes.h"
#include <stdint.h>
#include <stdbool.h>

// Functions
int utils_middle_of_3_int(int a, int b, int c);
uint32_t utils_crc32c(uint8_t *data, uint32_t len);
const char* utils_fault_to_string(sw_fault_code fault);
const char* utils_hw_type_to_string(HW_TYPE hw);

/**
 * A simple low pass filter.
 *
 * @param value
 * The filtered value.
 *
 * @param sample
 * Next sample.
 *
 * @param filter_constant
 * Filter constant. Range 0.0 to 1.0, where 1.0 gives the unfiltered value.
 */
#define UTILS_LP_FAST(value, sample, filter_constant)	(value -= (filter_constant) * ((value) - (sample)))

// Return the age of a timestamp in seconds
#define UTILS_AGE_S(x)		((float)chVTTimeElapsedSinceX(x) / (float)CH_CFG_ST_FREQUENCY)

#endif /* UTILS_H_ */
