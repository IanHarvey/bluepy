/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  NXP Semiconductors. All rights reserved.
 *
 */
#include <stdbool.h>
#include <inttypes.h>

#include "src/shared/io.h"
#include "src/shared/gatt-client.h"

struct bt_mics;
struct bt_micp;

typedef void (*bt_micp_ready_func_t)(struct bt_micp *micp, void *user_data);
typedef void (*bt_micp_destroy_func_t)(void *user_data);
typedef void (*bt_micp_debug_func_t)(const char *str, void *user_data);
typedef void (*bt_micp_func_t)(struct bt_micp *micp, void *user_data);

struct bt_micp_db {
	struct gatt_db *db;
	struct bt_mics *mics;
};

struct bt_mics {
	struct bt_micp_db *mdb;
	uint8_t mute_stat;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *ms;
	struct gatt_db_attribute *ms_ccc;
};

struct bt_micp {
	int ref_count;
	struct bt_micp_db *ldb;
	struct bt_micp_db *rdb;
	struct bt_gatt_client *client;
	struct bt_att *att;
	unsigned int mute_id;

	unsigned int idle_id;
	uint8_t mute;

	struct queue *notify;
	struct queue *pending;
	struct queue *ready_cbs;

	bt_micp_debug_func_t debug_func;
	bt_micp_destroy_func_t debug_destroy;

	void *debug_data;
	void *user_data;
};

struct bt_micp *bt_micp_ref(struct bt_micp *micp);
void bt_micp_unref(struct bt_micp *micp);

void bt_micp_add_db(struct gatt_db *db);

bool bt_micp_attach(struct bt_micp *micp, struct bt_gatt_client *client);
void bt_micp_detach(struct bt_micp *micp);

bool bt_micp_set_debug(struct bt_micp *micp, bt_micp_debug_func_t func,
		void *user_data, bt_micp_destroy_func_t destroy);

struct bt_att *bt_micp_get_att(struct bt_micp *micp);

bool bt_micp_set_user_data(struct bt_micp *micp, void *user_data);

/* session related functions */
unsigned int bt_micp_register(bt_micp_func_t attached, bt_micp_func_t detached,
					void *user_data);
unsigned int bt_micp_ready_register(struct bt_micp *micp,
				bt_micp_ready_func_t func, void *user_data,
				bt_micp_destroy_func_t destroy);
bool bt_micp_ready_unregister(struct bt_micp *micp, unsigned int id);

bool bt_micp_unregister(unsigned int id);
struct bt_micp *bt_micp_new(struct gatt_db *ldb, struct gatt_db *rdb);
struct bt_mics *micp_get_mics(struct bt_micp *micp);
