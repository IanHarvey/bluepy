// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"

#include "log.h"
#include "adapter.h"
#include "btd.h"

enum rfkill_type {
	RFKILL_TYPE_ALL = 0,
	RFKILL_TYPE_WLAN,
	RFKILL_TYPE_BLUETOOTH,
	RFKILL_TYPE_UWB,
	RFKILL_TYPE_WIMAX,
	RFKILL_TYPE_WWAN,
};

enum rfkill_operation {
	RFKILL_OP_ADD = 0,
	RFKILL_OP_DEL,
	RFKILL_OP_CHANGE,
	RFKILL_OP_CHANGE_ALL,
};

struct rfkill_event {
	uint32_t idx;
	uint8_t  type;
	uint8_t  op;
	uint8_t  soft;
	uint8_t  hard;
};
#define RFKILL_EVENT_SIZE_V1    8

static int get_adapter_id_for_rfkill(int rfkill_id)
{
	char sysname[PATH_MAX];
	int namefd;

	snprintf(sysname, sizeof(sysname) - 1,
			"/sys/class/rfkill/rfkill%u/name", rfkill_id);

	namefd = open(sysname, O_RDONLY);
	if (namefd < 0)
		return -1;

	memset(sysname, 0, sizeof(sysname));

	if (read(namefd, sysname, sizeof(sysname) - 1) < 4) {
		close(namefd);
		return -1;
	}

	close(namefd);

	if (g_str_has_prefix(sysname, "hci") == FALSE)
		return -1;

	return atoi(sysname + 3);
}

int rfkill_get_blocked(uint16_t index)
{
	int fd;
	int blocked = -1;

	fd = open("/dev/rfkill", O_RDWR);
	if (fd < 0) {
		DBG("Failed to open RFKILL control device");
		return -1;
	}

	while (1) {
		struct rfkill_event event = { 0 };
		int id;
		ssize_t len;

		len = read(fd, &event, sizeof(event));
		if (len < RFKILL_EVENT_SIZE_V1)
			break;

		id = get_adapter_id_for_rfkill(event.idx);

		if (index == id) {
			blocked = event.soft || event.hard;
			break;
		}
	}
	close(fd);

	return blocked;
}

static gboolean rfkill_event(GIOChannel *chan,
				GIOCondition cond, gpointer data)
{
	struct rfkill_event event = { 0 };
	struct btd_adapter *adapter;
	bool blocked = false;
	ssize_t len;
	int fd, id;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	fd = g_io_channel_unix_get_fd(chan);

	len = read(fd, &event, sizeof(event));
	if (len < 0) {
		if (errno == EAGAIN)
			return TRUE;
		return FALSE;
	}

	if (len < RFKILL_EVENT_SIZE_V1)
		return TRUE;

	DBG("RFKILL event idx %u type %u op %u soft %u hard %u",
					event.idx, event.type, event.op,
						event.soft, event.hard);

	if (event.soft || event.hard)
		blocked = true;

	if (event.op != RFKILL_OP_CHANGE)
		return TRUE;

	if (event.type != RFKILL_TYPE_BLUETOOTH &&
					event.type != RFKILL_TYPE_ALL)
		return TRUE;

	id = get_adapter_id_for_rfkill(event.idx);
	if (id < 0)
		return TRUE;

	adapter = adapter_find_by_id(id);
	if (!adapter)
		return TRUE;

	DBG("RFKILL unblock for hci%d", id);

	if (blocked)
		btd_adapter_set_blocked(adapter);
	else
		btd_adapter_restore_powered(adapter);

	return TRUE;
}

static guint watch = 0;

void rfkill_init(void)
{
	int fd;
	GIOChannel *channel;

	fd = open("/dev/rfkill", O_RDWR);
	if (fd < 0) {
		error("Failed to open RFKILL control device");
		return;
	}

	channel = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(channel, TRUE);

	watch = g_io_add_watch(channel,
				G_IO_IN | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
				rfkill_event, NULL);

	g_io_channel_unref(channel);
}

void rfkill_exit(void)
{
	if (watch == 0)
		return;

	g_source_remove(watch);
	watch = 0;
}
