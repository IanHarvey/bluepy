/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2015  Google Inc.
 *
 *
 */

struct btd_gatt_database;

struct btd_gatt_database *btd_gatt_database_new(struct btd_adapter *adapter);
void btd_gatt_database_destroy(struct btd_gatt_database *database);

struct gatt_db *btd_gatt_database_get_db(struct btd_gatt_database *database);
void btd_gatt_database_att_disconnected(struct btd_gatt_database *database,
						struct btd_device *device);
void btd_gatt_database_server_connected(struct btd_gatt_database *database,
						struct bt_gatt_server *server);

void btd_gatt_database_restore_svc_chng_ccc(struct btd_gatt_database *database);
