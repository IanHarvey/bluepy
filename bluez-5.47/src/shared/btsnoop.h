/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012-2014  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#define BTSNOOP_TYPE_INVALID		0
#define BTSNOOP_TYPE_HCI		1001
#define BTSNOOP_TYPE_UART		1002
#define BTSNOOP_TYPE_BCSP		1003
#define BTSNOOP_TYPE_3WIRE		1004
#define BTSNOOP_TYPE_MONITOR		2001
#define BTSNOOP_TYPE_SIMULATOR		2002

#define BTSNOOP_FLAG_PKLG_SUPPORT	(1 << 0)

#define BTSNOOP_OPCODE_NEW_INDEX	0
#define BTSNOOP_OPCODE_DEL_INDEX	1
#define BTSNOOP_OPCODE_COMMAND_PKT	2
#define BTSNOOP_OPCODE_EVENT_PKT	3
#define BTSNOOP_OPCODE_ACL_TX_PKT	4
#define BTSNOOP_OPCODE_ACL_RX_PKT	5
#define BTSNOOP_OPCODE_SCO_TX_PKT	6
#define BTSNOOP_OPCODE_SCO_RX_PKT	7

#define BTSNOOP_MAX_PACKET_SIZE		(1486 + 4)

struct btsnoop_opcode_new_index {
	uint8_t  type;
	uint8_t  bus;
	uint8_t  bdaddr[6];
	char     name[8];
} __attribute__((packed));

struct btsnoop;

struct btsnoop *btsnoop_open(const char *path, unsigned long flags);
struct btsnoop *btsnoop_create(const char *path, uint32_t type);

struct btsnoop *btsnoop_ref(struct btsnoop *btsnoop);
void btsnoop_unref(struct btsnoop *btsnoop);

uint32_t btsnoop_get_type(struct btsnoop *btsnoop);

bool btsnoop_write(struct btsnoop *btsnoop, struct timeval *tv,
			uint32_t flags, const void *data, uint16_t size);
bool btsnoop_write_hci(struct btsnoop *btsnoop, struct timeval *tv,
					uint16_t index, uint16_t opcode,
					const void *data, uint16_t size);
bool btsnoop_write_phy(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t frequency, const void *data, uint16_t size);

bool btsnoop_read_hci(struct btsnoop *btsnoop, struct timeval *tv,
					uint16_t *index, uint16_t *opcode,
					void *data, uint16_t *size);
bool btsnoop_read_phy(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t *frequency, void *data, uint16_t *size);
