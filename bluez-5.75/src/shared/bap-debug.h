/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  Intel Corporation.
 */

bool bt_bap_debug_caps(void *data, size_t len, util_debug_func_t func,
						void *user_data);
bool bt_bap_debug_config(void *data, size_t len, util_debug_func_t func,
						void *user_data);
bool bt_bap_debug_metadata(void *data, size_t len, util_debug_func_t func,
						void *user_data);
