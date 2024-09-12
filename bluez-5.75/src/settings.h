/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation.
 *
 */

int btd_settings_gatt_db_load(struct gatt_db *db, const char *filename);
void btd_settings_gatt_db_store(struct gatt_db *db, const char *filename);
