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

struct bt_mcp;
struct bt_mcp_db;
struct bt_mcp_session_info;

typedef void (*bt_mcp_debug_func_t)(const char *str, void *user_data);
typedef void (*bt_mcp_destroy_func_t)(void *user_data);

struct bt_mcp_event_callback {
	void (*player_name)(struct bt_mcp *mcp,  const uint8_t *value,
					uint16_t length);
	void (*track_changed)(struct bt_mcp *mcp);
	void (*track_title)(struct bt_mcp *mcp, const uint8_t *value,
					uint16_t length);
	void (*track_duration)(struct bt_mcp *mcp, int32_t duration);
	void (*track_position)(struct bt_mcp *mcp, int32_t position);
	void (*playback_speed)(struct bt_mcp *mcp, int8_t speed);
	void (*seeking_speed)(struct bt_mcp *mcp, int8_t speed);
	void (*play_order)(struct bt_mcp *mcp, uint8_t order);
	void (*play_order_supported)(struct bt_mcp *mcp,
					uint16_t order_supported);
	void (*media_state)(struct bt_mcp *mcp, uint8_t state);
	void (*content_control_id)(struct bt_mcp *mcp, uint8_t cc_id);
};

void bt_mcp_set_event_callbacks(struct bt_mcp *mcp,
				const struct bt_mcp_event_callback *cbs,
				void *user_data);

bool bt_mcp_set_debug(struct bt_mcp *mcp, bt_mcp_debug_func_t cb,
			void *user_data, bt_mcp_destroy_func_t destroy);

void bt_mcp_register(struct gatt_db *db);
bool bt_mcp_attach(struct bt_mcp *mcp, struct bt_gatt_client *client);
void bt_mcp_detach(struct bt_mcp *mcp);

struct bt_mcp *bt_mcp_new(struct gatt_db *ldb, struct gatt_db *rdb);
struct bt_mcp *bt_mcp_ref(struct bt_mcp *mcp);
void bt_mcp_unref(struct bt_mcp *mcp);

bool bt_mcp_set_user_data(struct bt_mcp *mcp, void *user_data);
void *bt_mcp_get_user_data(struct bt_mcp *mcp);

unsigned int bt_mcp_play(struct bt_mcp *mcp);
unsigned int bt_mcp_pause(struct bt_mcp *mcp);
unsigned int bt_mcp_stop(struct bt_mcp *mcp);
