/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  Intel Corporation
 *
 *
 */

#define BTD_DEVICE_SET_INTERFACE	"org.bluez.DeviceSet1"

struct btd_device_set;

struct btd_device_set *btd_set_add_device(struct btd_device *device,
						uint8_t *ltk, uint8_t sirk[16],
						uint8_t size);
bool btd_set_remove_device(struct btd_device_set *set,
						struct btd_device *device);
const char *btd_set_get_path(struct btd_device_set *set);
