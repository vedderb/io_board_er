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

#ifndef COMM_CAN_H_
#define COMM_CAN_H_

#include "conf_general.h"

// Settings
#define CAN_STATUS_MSGS_TO_STORE	10

// Functions
void comm_can_init(void);
void comm_can_set_baud(CAN_BAUD baud);
void comm_can_transmit_eid(uint32_t id, const uint8_t *data, uint8_t len);
void comm_can_transmit_sid(uint32_t id, const uint8_t *data, uint8_t len);
void comm_can_set_sid_rx_callback(void (*p_func)(uint32_t id, uint8_t *data, uint8_t len));
void comm_can_send_buffer(uint8_t controller_id, uint8_t *data, unsigned int len, uint8_t send);

can_status_msg *comm_can_get_status_msg_index(int index);
can_status_msg *comm_can_get_status_msg_id(int id);
can_status_msg_2 *comm_can_get_status_msg_2_index(int index);
can_status_msg_2 *comm_can_get_status_msg_2_id(int id);
can_status_msg_3 *comm_can_get_status_msg_3_index(int index);
can_status_msg_3 *comm_can_get_status_msg_3_id(int id);
can_status_msg_4 *comm_can_get_status_msg_4_index(int index);
can_status_msg_4 *comm_can_get_status_msg_4_id(int id);
can_status_msg_5 *comm_can_get_status_msg_5_index(int index);
can_status_msg_5 *comm_can_get_status_msg_5_id(int id);

bool comm_can_ping(uint8_t controller_id, HW_TYPE *hw_type);

#endif /* COMM_CAN_H_ */