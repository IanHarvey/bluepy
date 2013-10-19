/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *  Copyright (C) 2011  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#define EIR_FLAGS                   0x01  /* flags */
#define EIR_UUID16_SOME             0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL              0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME             0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL              0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME            0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL             0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT              0x08  /* shortened local name */
#define EIR_NAME_COMPLETE           0x09  /* complete local name */
#define EIR_TX_POWER                0x0A  /* transmit power level */
#define EIR_CLASS_OF_DEV            0x0D  /* Class of Device */
#define EIR_SSP_HASH                0x0E  /* SSP Hash */
#define EIR_SSP_RANDOMIZER          0x0F  /* SSP Randomizer */
#define EIR_DEVICE_ID               0x10  /* device ID */
#define EIR_GAP_APPEARANCE          0x19  /* GAP appearance */

struct eir_data {
	GSList *services;
	int flags;
	char *name;
	uint32_t class;
	uint16_t appearance;
	gboolean name_complete;
	int8_t tx_power;
	uint8_t *hash;
	uint8_t *randomizer;
	bdaddr_t addr;
};

void eir_data_free(struct eir_data *eir);
int eir_parse(struct eir_data *eir, const uint8_t *eir_data, uint8_t eir_len);
int eir_parse_oob(struct eir_data *eir, uint8_t *eir_data, uint16_t eir_len);
int eir_create_oob(const bdaddr_t *addr, const char *name, uint32_t cod,
			const uint8_t *hash, const uint8_t *randomizer,
			uint16_t did_vendor, uint16_t did_product,
			uint16_t did_version, uint16_t did_source,
			sdp_list_t *uuids, uint8_t *data);
