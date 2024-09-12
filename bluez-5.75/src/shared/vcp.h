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

#include "src/shared/io.h"

#define BT_VCP_RENDERER			0x01
#define	BT_VCP_CONTROLLER		0x02

#define BT_VCP_RELATIVE_VOL_DOWN	0x00
#define BT_VCP_RELATIVE_VOL_UP		0x01
#define BT_VCP_UNMUTE_RELATIVE_VOL_DOWN	0x02
#define BT_VCP_UNMUTE_RELATIVE_VOL_UP	0x03
#define BT_VCP_SET_ABOSULTE_VOL		0x04
#define BT_VCP_UNMUTE			0x05
#define BT_VCP_MUTE			0x06

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct bt_vcp;

typedef void (*bt_vcp_destroy_func_t)(void *user_data);
typedef void (*bt_vcp_debug_func_t)(const char *str, void *user_data);
typedef void (*bt_vcp_func_t)(struct bt_vcp *vcp, void *user_data);

struct bt_vcp *bt_vcp_ref(struct bt_vcp *vcp);
void bt_vcp_unref(struct bt_vcp *vcp);

void bt_vcp_add_db(struct gatt_db *db);

bool bt_vcp_attach(struct bt_vcp *vcp, struct bt_gatt_client *client);
void bt_vcp_detach(struct bt_vcp *vcp);

bool bt_vcp_set_debug(struct bt_vcp *vcp, bt_vcp_debug_func_t cb,
			void *user_data, bt_vcp_destroy_func_t destroy);

struct bt_att *bt_vcp_get_att(struct bt_vcp *vcp);

bool bt_vcp_set_user_data(struct bt_vcp *vcp, void *user_data);

/* Session related function */
unsigned int bt_vcp_register(bt_vcp_func_t added, bt_vcp_func_t removed,
							void *user_data);
bool bt_vcp_unregister(unsigned int id);
struct bt_vcp *bt_vcp_new(struct gatt_db *ldb, struct gatt_db *rdb);
