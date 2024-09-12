/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020  Intel Corporation. All rights reserved.
 *
 */

#include <stdbool.h>
#include <inttypes.h>

#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct bt_ccp;
struct bt_ccp_db;
struct bt_ccp_session_info;

typedef void (*bt_ccp_debug_func_t)(const char *str, void *user_data);
typedef void (*bt_ccp_destroy_func_t)(void *user_data);

struct bt_ccp_event_callback {
	void (*call_state)(struct bt_ccp *ccp,  const uint8_t *value,
			   uint16_t length);
};

void bt_ccp_set_event_callbacks(struct bt_ccp *ccp,
				const struct bt_ccp_event_callback *cbs,
				void *user_data);

bool bt_ccp_set_debug(struct bt_ccp *ccp, bt_ccp_debug_func_t cb,
		      void *user_data, bt_ccp_destroy_func_t destroy);

void bt_ccp_register(struct gatt_db *db);
bool bt_ccp_attach(struct bt_ccp *ccp, struct bt_gatt_client *client);
void bt_ccp_detach(struct bt_ccp *ccp);

struct bt_ccp *bt_ccp_new(struct gatt_db *ldb, struct gatt_db *rdb);
struct bt_ccp *bt_ccp_ref(struct bt_ccp *ccp);
void bt_ccp_unref(struct bt_ccp *ccp);

bool bt_ccp_set_user_data(struct bt_ccp *ccp, void *user_data);
void *bt_ccp_get_user_data(struct bt_ccp *ccp);
