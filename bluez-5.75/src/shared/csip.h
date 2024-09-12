/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *
 */

#include <stdbool.h>
#include <inttypes.h>

#include "src/shared/io.h"

#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct bt_csip;

enum {
	BT_CSIP_SIRK_ENCRYPT = 0x00,
	BT_CSIP_SIRK_CLEARTEXT = 0x01
};

typedef void (*bt_csip_ready_func_t)(struct bt_csip *csip, void *user_data);
typedef void (*bt_csip_destroy_func_t)(void *user_data);
typedef void (*bt_csip_debug_func_t)(const char *str, void *user_data);
typedef void (*bt_csip_func_t)(struct bt_csip *csip, void *user_data);
typedef bool (*bt_csip_encrypt_func_t)(struct bt_att *att, uint8_t k[16]);
typedef bool (*bt_csip_sirk_func_t)(struct bt_csip *csip, uint8_t type,
				    uint8_t k[16], uint8_t size, uint8_t rank,
				    void *user_data);

struct bt_csip *bt_csip_ref(struct bt_csip *csip);
void bt_csip_unref(struct bt_csip *csip);

bool bt_csip_attach(struct bt_csip *csip, struct bt_gatt_client *client);
void bt_csip_detach(struct bt_csip *csip);

bool bt_csip_set_debug(struct bt_csip *csip, bt_csip_debug_func_t func,
			void *user_data, bt_csip_destroy_func_t destroy);

struct bt_att *bt_csip_get_att(struct bt_csip *csip);

bool bt_csip_set_user_data(struct bt_csip *csip, void *user_data);

/* Session related function */
unsigned int bt_csip_register(bt_csip_func_t added, bt_csip_func_t removed,
							void *user_data);
bool bt_csip_unregister(unsigned int id);
struct bt_csip *bt_csip_new(struct gatt_db *ldb, struct gatt_db *rdb);

bool bt_csip_set_sirk(struct bt_csip *csip, bool encrypt,
				uint8_t k[16], uint8_t size, uint8_t rank,
				bt_csip_encrypt_func_t func);

bool bt_csip_get_sirk(struct bt_csip *csip, uint8_t *type,
				uint8_t k[16], uint8_t *size, uint8_t *rank);

unsigned int bt_csip_ready_register(struct bt_csip *csip,
				bt_csip_ready_func_t func, void *user_data,
				bt_csip_destroy_func_t destroy);
bool bt_csip_ready_unregister(struct bt_csip *csip, unsigned int id);
