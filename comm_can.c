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

#include "comm_can.h"
#include "crc.h"
#include "packet.h"
#include "commands.h"
#include "buffer.h"
#include "adc_read.h"
#include "main.h"
#include <string.h>

// Settings
#define RX_FRAMES_SIZE				100
#define RX_BUFFER_SIZE				PACKET_MAX_PL_LEN

// Private variables
static can_status_msg stat_msgs[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_2 stat_msgs_2[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_3 stat_msgs_3[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_4 stat_msgs_4[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_5 stat_msgs_5[CAN_STATUS_MSGS_TO_STORE];

static mutex_t can_mtx;
static CANRxFrame rx_frames[RX_FRAMES_SIZE];
static int rx_frame_read;
static int rx_frame_write;
static thread_t *process_tp = 0;
static thread_t *ping_tp = 0;
static volatile HW_TYPE ping_hw_last = HW_TYPE_VESC;
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static unsigned int rx_buffer_last_id;

// Threads
static THD_WORKING_AREA(cancom_read_thread_wa, 512);
static THD_WORKING_AREA(cancom_process_thread_wa, 4096);
static THD_WORKING_AREA(cancom_status_thread_wa, 512);
static THD_FUNCTION(cancom_read_thread, arg);
static THD_FUNCTION(cancom_process_thread, arg);
static THD_FUNCTION(cancom_status_thread, arg);

// Private functions
static void set_timing(int brp, int ts1, int ts2);
static void send_packet_wrapper(unsigned char *data, unsigned int len);
static void decode_msg(uint32_t eid, uint8_t *data8, int len, bool is_replaced);

/*
 * 500KBaud, automatic wakeup, automatic recover
 * from abort mode.
 * See section 22.7.7 on the STM32 reference manual.
 */
static CANConfig cancfg = {
		CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
		CAN_BTR_SJW(3) | CAN_BTR_TS2(2) |
		CAN_BTR_TS1(9) | CAN_BTR_BRP(5)
};

// Function pointers
static void(*sid_callback)(uint32_t id, uint8_t *data, uint8_t len) = 0;

void comm_can_init(void) {
	chMtxObjectInit(&can_mtx);

	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		stat_msgs[i].id = -1;
		stat_msgs_2[i].id = -1;
		stat_msgs_3[i].id = -1;
		stat_msgs_4[i].id = -1;
		stat_msgs_5[i].id = -1;
	}

	palSetLineMode(LINE_CAN_RX, PAL_MODE_ALTERNATE(HW_CAN_AF));
	palSetLineMode(LINE_CAN_TX, PAL_MODE_ALTERNATE(HW_CAN_AF));

	canStart(&HW_CAN_DEV, &cancfg);

	chThdCreateStatic(cancom_read_thread_wa, sizeof(cancom_read_thread_wa), NORMALPRIO + 2,
			cancom_read_thread, NULL);
	chThdCreateStatic(cancom_process_thread_wa, sizeof(cancom_process_thread_wa), NORMALPRIO + 1,
			cancom_process_thread, NULL);
	chThdCreateStatic(cancom_status_thread_wa, sizeof(cancom_status_thread_wa), NORMALPRIO,
			cancom_status_thread, NULL);
}

void comm_can_set_baud(CAN_BAUD baud) {
	switch (baud) {
	case CAN_BAUD_125K:	set_timing(15, 14, 4); break;
	case CAN_BAUD_250K:	set_timing(7, 14, 4); break;
	case CAN_BAUD_500K:	set_timing(5, 9, 2); break;
	case CAN_BAUD_1M:	set_timing(2, 9, 2); break;
	case CAN_BAUD_10K:	set_timing(299, 10, 1); break;
	case CAN_BAUD_20K:	set_timing(149, 10, 1); break;
	case CAN_BAUD_50K:	set_timing(59, 10, 1); break;
	case CAN_BAUD_75K:	set_timing(39, 10, 1); break;
	default: break;
	}
}

void comm_can_transmit_eid(uint32_t id, const uint8_t *data, uint8_t len) {
	if (len > 8) {
		len = 8;
	}

	CANTxFrame txmsg;
	txmsg.IDE = CAN_IDE_EXT;
	txmsg.EID = id;
	txmsg.RTR = CAN_RTR_DATA;
	txmsg.DLC = len;
	memcpy(txmsg.data8, data, len);

	chMtxLock(&can_mtx);
	canTransmit(&HW_CAN_DEV, CAN_ANY_MAILBOX, &txmsg, TIME_MS2I(5));
	chMtxUnlock(&can_mtx);
}

void comm_can_transmit_sid(uint32_t id, const uint8_t *data, uint8_t len) {
	if (len > 8) {
		len = 8;
	}

	CANTxFrame txmsg;
	txmsg.IDE = CAN_IDE_STD;
	txmsg.SID = id;
	txmsg.RTR = CAN_RTR_DATA;
	txmsg.DLC = len;
	memcpy(txmsg.data8, data, len);

	chMtxLock(&can_mtx);
	canTransmit(&HW_CAN_DEV, CAN_ANY_MAILBOX, &txmsg, TIME_MS2I(5));
	chMtxUnlock(&can_mtx);
}

void comm_can_set_sid_rx_callback(void (*p_func)(uint32_t id, uint8_t *data, uint8_t len)) {
	sid_callback = p_func;
}

void comm_can_send_buffer(uint8_t controller_id, uint8_t *data, unsigned int len, uint8_t send) {
	uint8_t send_buffer[8];

	if (len <= 6) {
		uint32_t ind = 0;
		send_buffer[ind++] = CONTROLLER_ID;
		send_buffer[ind++] = send;
		memcpy(send_buffer + ind, data, len);
		ind += len;
		comm_can_transmit_eid(controller_id |
				((uint32_t)CAN_PACKET_PROCESS_SHORT_BUFFER << 8), send_buffer, ind);
	} else {
		unsigned int end_a = 0;
		for (unsigned int i = 0;i < len;i += 7) {
			if (i > 255) {
				break;
			}

			end_a = i + 7;

			uint8_t send_len = 7;
			send_buffer[0] = i;

			if ((i + 7) <= len) {
				memcpy(send_buffer + 1, data + i, send_len);
			} else {
				send_len = len - i;
				memcpy(send_buffer + 1, data + i, send_len);
			}

			comm_can_transmit_eid(controller_id |
					((uint32_t)CAN_PACKET_FILL_RX_BUFFER << 8), send_buffer, send_len + 1);
		}

		for (unsigned int i = end_a;i < len;i += 6) {
			uint8_t send_len = 6;
			send_buffer[0] = i >> 8;
			send_buffer[1] = i & 0xFF;

			if ((i + 6) <= len) {
				memcpy(send_buffer + 2, data + i, send_len);
			} else {
				send_len = len - i;
				memcpy(send_buffer + 2, data + i, send_len);
			}

			comm_can_transmit_eid(controller_id |
					((uint32_t)CAN_PACKET_FILL_RX_BUFFER_LONG << 8), send_buffer, send_len + 2);
		}

		uint32_t ind = 0;
		send_buffer[ind++] = CONTROLLER_ID;
		send_buffer[ind++] = send;
		send_buffer[ind++] = len >> 8;
		send_buffer[ind++] = len & 0xFF;
		unsigned short crc = crc16(data, len);
		send_buffer[ind++] = (uint8_t)(crc >> 8);
		send_buffer[ind++] = (uint8_t)(crc & 0xFF);

		comm_can_transmit_eid(controller_id |
				((uint32_t)CAN_PACKET_PROCESS_RX_BUFFER << 8), send_buffer, ind++);
	}
}

/**
 * Get status message by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg *comm_can_get_status_msg_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs[index];
	} else {
		return 0;
	}
}

/**
 * Get status message by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg *comm_can_get_status_msg_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs[i].id == id) {
			return &stat_msgs[i];
		}
	}

	return 0;
}

/**
 * Get status message 2 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_2 *comm_can_get_status_msg_2_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_2[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 2 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_2 *comm_can_get_status_msg_2_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_2[i].id == id) {
			return &stat_msgs_2[i];
		}
	}

	return 0;
}

/**
 * Get status message 3 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_3 *comm_can_get_status_msg_3_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_3[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 3 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_3 *comm_can_get_status_msg_3_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_3[i].id == id) {
			return &stat_msgs_3[i];
		}
	}

	return 0;
}

/**
 * Get status message 4 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_4 *comm_can_get_status_msg_4_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_4[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 4 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_4 *comm_can_get_status_msg_4_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_4[i].id == id) {
			return &stat_msgs_4[i];
		}
	}

	return 0;
}

/**
 * Get status message 5 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_5 *comm_can_get_status_msg_5_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_5[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 5 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_5 *comm_can_get_status_msg_5_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_5[i].id == id) {
			return &stat_msgs_5[i];
		}
	}

	return 0;
}

/**
 * Check if a VESC on the CAN-bus responds.
 *
 * @param controller_id
 * The ID of the VESC.
 *
 * @param hw_type
 * The hardware type of the CAN device.
 *
 * @return
 * True for success, false otherwise.
 */
bool comm_can_ping(uint8_t controller_id, HW_TYPE *hw_type) {
	ping_tp = chThdGetSelfX();
	chEvtGetAndClearEvents(ALL_EVENTS);

	uint8_t buffer[1];
	buffer[0] = CONTROLLER_ID;
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_PING << 8), buffer, 1);

	int ret = chEvtWaitAnyTimeout(1 << 29, TIME_MS2I(10));
	ping_tp = 0;

	if (ret != 0) {
		if (hw_type) {
			*hw_type = ping_hw_last;
		}
	}

	return ret != 0;
}

static THD_FUNCTION(cancom_read_thread, arg) {
	(void)arg;
	chRegSetThreadName("CAN read");

	event_listener_t el;
	CANRxFrame rxmsg;

	chEvtRegister(&HW_CAN_DEV.rxfull_event, &el, 0);

	while(!chThdShouldTerminateX()) {
		if (chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(10)) == 0) {
			continue;
		}

		msg_t result = canReceive(&HW_CAN_DEV, CAN_ANY_MAILBOX, &rxmsg, TIME_IMMEDIATE);

		while (result == MSG_OK) {
			rx_frames[rx_frame_write++] = rxmsg;
			if (rx_frame_write == RX_FRAMES_SIZE) {
				rx_frame_write = 0;
			}

			chEvtSignal(process_tp, (eventmask_t) 1);
//			main_reset_sleep_timer();

			result = canReceive(&HW_CAN_DEV, CAN_ANY_MAILBOX, &rxmsg, TIME_IMMEDIATE);
		}
	}

	chEvtUnregister(&HW_CAN_DEV.rxfull_event, &el);
}

static THD_FUNCTION(cancom_process_thread, arg) {
	(void)arg;

	chRegSetThreadName("CAN process");
	process_tp = chThdGetSelfX();

	for(;;) {
		chEvtWaitAny((eventmask_t) 1);

		while (rx_frame_read != rx_frame_write) {
			CANRxFrame rxmsg = rx_frames[rx_frame_read++];
			if (rx_frame_read == RX_FRAMES_SIZE) {
				rx_frame_read = 0;
			}

			if (rxmsg.IDE == CAN_IDE_EXT) {
				decode_msg(rxmsg.EID, rxmsg.data8, rxmsg.DLC, false);
			} else {
				if (sid_callback) {
					sid_callback(rxmsg.SID, rxmsg.data8, rxmsg.DLC);
				}
			}
		}
	}
}

static THD_FUNCTION(cancom_status_thread, arg) {
	(void)arg;
	chRegSetThreadName("CAN status");

	for(;;) {
		int32_t send_index = 0;
		uint8_t buffer[8];

		buffer_append_float16(buffer, ADC_GET_V_DIV(0), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(1), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(2), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(3), 1e2, &send_index);
		comm_can_transmit_eid(CONTROLLER_ID | ((uint32_t)CAN_PACKET_IO_BOARD_ADC_1_TO_4 << 8), buffer, send_index);

		send_index = 0;
		buffer_append_float16(buffer, ADC_GET_V_DIV(4), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(5), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(6), 1e2, &send_index);
		buffer_append_float16(buffer, ADC_GET_V_DIV(7), 1e2, &send_index);
		comm_can_transmit_eid(CONTROLLER_ID | ((uint32_t)CAN_PACKET_IO_BOARD_ADC_5_TO_8 << 8), buffer, send_index);

		send_index = 0;
		buffer[send_index++] =
				LIM_SW_READ(LINE_LIM_SW_1) << 0 |
				LIM_SW_READ(LINE_LIM_SW_2) << 1 |
				LIM_SW_READ(LINE_LIM_SW_3) << 2 |
				LIM_SW_READ(LINE_LIM_SW_4) << 3;
		comm_can_transmit_eid(CONTROLLER_ID | ((uint32_t)CAN_PACKET_IO_BOARD_DIGITAL_IN << 8), buffer, send_index);

		systime_t sleep_time = CH_CFG_ST_FREQUENCY / CAN_STATUS_RATE_HZ;
		if (sleep_time == 0) {
			sleep_time = 1;
		}

		chThdSleep(sleep_time);
	}
}

static void send_packet_wrapper(unsigned char *data, unsigned int len) {
	comm_can_send_buffer(rx_buffer_last_id, data, len, 1);
}

static void decode_msg(uint32_t eid, uint8_t *data8, int len, bool is_replaced) {
	int32_t ind = 0;
	unsigned int rxbuf_len;
	unsigned int rxbuf_ind;
	uint8_t crc_low;
	uint8_t crc_high;
	uint8_t commands_send;

	uint8_t id = eid & 0xFF;
	CAN_PACKET_ID cmd = eid >> 8;

	if (id == 255 || id == CONTROLLER_ID) {
		switch (cmd) {
		case CAN_PACKET_FILL_RX_BUFFER:
			memcpy(rx_buffer + data8[0], data8 + 1, len - 1);
			break;

		case CAN_PACKET_FILL_RX_BUFFER_LONG:
			rxbuf_ind = (unsigned int)data8[0] << 8;
			rxbuf_ind |= data8[1];
			if (rxbuf_ind < RX_BUFFER_SIZE) {
				memcpy(rx_buffer + rxbuf_ind, data8 + 2, len - 2);
			}
			break;

		case CAN_PACKET_PROCESS_RX_BUFFER:
			ind = 0;
			rx_buffer_last_id = data8[ind++];
			commands_send = data8[ind++];
			rxbuf_len = (unsigned int)data8[ind++] << 8;
			rxbuf_len |= (unsigned int)data8[ind++];

			if (rxbuf_len > RX_BUFFER_SIZE) {
				break;
			}

			crc_high = data8[ind++];
			crc_low = data8[ind++];

			if (crc16(rx_buffer, rxbuf_len)
					== ((unsigned short) crc_high << 8
							| (unsigned short) crc_low)) {

				if (is_replaced) {
					if (rx_buffer[0] == COMM_JUMP_TO_BOOTLOADER ||
							rx_buffer[0] == COMM_ERASE_NEW_APP ||
							rx_buffer[0] == COMM_WRITE_NEW_APP_DATA ||
							rx_buffer[0] == COMM_WRITE_NEW_APP_DATA_LZO ||
							rx_buffer[0] == COMM_ERASE_BOOTLOADER) {
						break;
					}
				}

				switch (commands_send) {
				case 0:
					commands_process_packet(rx_buffer, rxbuf_len, send_packet_wrapper);
					break;
				case 1:
					commands_send_packet(rx_buffer, rxbuf_len);
					break;
				case 2:
					commands_process_packet(rx_buffer, rxbuf_len, 0);
					break;
				default:
					break;
				}
			}
			break;

		case CAN_PACKET_PROCESS_SHORT_BUFFER:
			ind = 0;
			rx_buffer_last_id = data8[ind++];
			commands_send = data8[ind++];

			if (is_replaced) {
				if (data8[ind] == COMM_JUMP_TO_BOOTLOADER ||
						data8[ind] == COMM_ERASE_NEW_APP ||
						data8[ind] == COMM_WRITE_NEW_APP_DATA ||
						data8[ind] == COMM_WRITE_NEW_APP_DATA_LZO ||
						data8[ind] == COMM_ERASE_BOOTLOADER) {
					break;
				}
			}

			switch (commands_send) {
			case 0:
				commands_process_packet(data8 + ind, len - ind, send_packet_wrapper);
				break;
			case 1:
				commands_send_packet(data8 + ind, len - ind);
				break;
			case 2:
				commands_process_packet(data8 + ind, len - ind, 0);
				break;
			default:
				break;
			}
			break;

			case CAN_PACKET_PING: {
				uint8_t buffer[2];
				buffer[0] = CONTROLLER_ID;
				buffer[1] = HW_TYPE_VESC_BMS;
				comm_can_transmit_eid(data8[0] |
						((uint32_t)CAN_PACKET_PONG << 8), buffer, 2);
			} break;

			case CAN_PACKET_PONG:
				// data8[0]; // Sender ID
				if (ping_tp) {
					if (len >= 2) {
						ping_hw_last = data8[1];
					} else {
						ping_hw_last = HW_TYPE_VESC_BMS;
					}
					chEvtSignal(ping_tp, 1 << 29);
				}
				break;

			case CAN_PACKET_SHUTDOWN: {
				// TODO: Implement when hw has power switch
			} break;

			case CAN_PACKET_IO_BOARD_SET_OUTPUT_DIGITAL: {
				int ind = 0;
				int ch_first = data8[ind++];
				int ch_num = data8[ind++];
				uint64_t channels = 0;

				while (ind < len) {
					channels |= (uint64_t)data8[ind] << (8 * (ind - 2));
					ind++;
				}

				for (int i = 0;i < ch_num;i++) {
					main_set_digital_out(i + ch_first, (channels >> i) & 1);
				}
			} break;

			case CAN_PACKET_IO_BOARD_SET_OUTPUT_PWM: {
				int32_t ind = 0;
				int ch_first = data8[ind++];

				while (ind < len) {
					main_set_pwm_out(ch_first++, buffer_get_float16(data8, 1e3, &ind));
				}
			} break;

			default:
				break;
		}
	}

	switch (cmd) {
	case CAN_PACKET_STATUS:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg *stat_tmp = &stat_msgs[i];
			if (stat_tmp->id == id || stat_tmp->id == -1) {
				ind = 0;
				stat_tmp->id = id;
				stat_tmp->rx_time = chVTGetSystemTime();
				stat_tmp->rpm = (float)buffer_get_int32(data8, &ind);
				stat_tmp->current = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp->duty = (float)buffer_get_int16(data8, &ind) / 1000.0;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_2:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_2 *stat_tmp_2 = &stat_msgs_2[i];
			if (stat_tmp_2->id == id || stat_tmp_2->id == -1) {
				ind = 0;
				stat_tmp_2->id = id;
				stat_tmp_2->rx_time = chVTGetSystemTime();
				stat_tmp_2->amp_hours = (float)buffer_get_int32(data8, &ind) / 1e4;
				stat_tmp_2->amp_hours_charged = (float)buffer_get_int32(data8, &ind) / 1e4;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_3:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_3 *stat_tmp_3 = &stat_msgs_3[i];
			if (stat_tmp_3->id == id || stat_tmp_3->id == -1) {
				ind = 0;
				stat_tmp_3->id = id;
				stat_tmp_3->rx_time = chVTGetSystemTime();
				stat_tmp_3->watt_hours = (float)buffer_get_int32(data8, &ind) / 1e4;
				stat_tmp_3->watt_hours_charged = (float)buffer_get_int32(data8, &ind) / 1e4;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_4:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_4 *stat_tmp_4 = &stat_msgs_4[i];
			if (stat_tmp_4->id == id || stat_tmp_4->id == -1) {
				ind = 0;
				stat_tmp_4->id = id;
				stat_tmp_4->rx_time = chVTGetSystemTime();
				stat_tmp_4->temp_fet = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->temp_motor = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->current_in = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->pid_pos_now = (float)buffer_get_int16(data8, &ind) / 50.0;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_5:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_5 *stat_tmp_5 = &stat_msgs_5[i];
			if (stat_tmp_5->id == id || stat_tmp_5->id == -1) {
				ind = 0;
				stat_tmp_5->id = id;
				stat_tmp_5->rx_time = chVTGetSystemTime();
				stat_tmp_5->tacho_value = buffer_get_int32(data8, &ind);
				stat_tmp_5->v_in = (float)buffer_get_int16(data8, &ind) / 1e1;
				break;
			}
		}
		break;

	default:
		break;
	}
}

/**
 * Set the CAN timing. The CAN is clocked at 80 MHz, and the baud rate can be
 * calculated with
 *
 * 42000000 / ((brp + 1) * (ts1 + ts2 + 3))
 *
 * ts1 should be larger than ts2 in general to take the sample after the
 * signal had time to stabilize.
 *
 * @param brp
 * Prescaler.
 *
 * @param ts1
 * TS1.
 *
 * @param ts2
 * TS2.
 */
static void set_timing(int brp, int ts1, int ts2) {
	brp &= 0b1111111111;
	ts1 &= 0b1111;
	ts2 &= 0b111;

	cancfg.btr = CAN_BTR_SJW(3) | CAN_BTR_TS2(ts2) |
		CAN_BTR_TS1(ts1) | CAN_BTR_BRP(brp);

	canStop(&HW_CAN_DEV);
	canStart(&HW_CAN_DEV, &cancfg);
}
