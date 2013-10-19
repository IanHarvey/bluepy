/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <glib.h>

#include "monitor/bt.h"
#include "emulator/btdev.h"
#include "emulator/bthost.h"

#include "hciemu.h"

struct hciemu {
	gint ref_count;
	enum btdev_type btdev_type;
	struct bthost *host_stack;
	struct btdev *master_dev;
	struct btdev *client_dev;
	guint host_source;
	guint master_source;
	guint client_source;
	GList *post_command_hooks;
};

struct hciemu_command_hook {
	hciemu_command_func_t function;
	void *user_data;
};

static void destroy_command_hook(gpointer data, gpointer user_data)
{
	struct hciemu_command_hook *hook = data;

	g_free(hook);
}

static void master_command_callback(uint16_t opcode,
				const void *data, uint8_t len,
				btdev_callback callback, void *user_data)
{
	struct hciemu *hciemu = user_data;
	GList *list;

	btdev_command_default(callback);

	for (list = g_list_first(hciemu->post_command_hooks); list;
						list = g_list_next(list)) {
		struct hciemu_command_hook *hook = list->data;

		if (hook->function)
			hook->function(opcode, data, len, hook->user_data);
	}
}

static void client_command_callback(uint16_t opcode,
				const void *data, uint8_t len,
				btdev_callback callback, void *user_data)
{
	btdev_command_default(callback);
}

static void write_callback(const void *data, uint16_t len, void *user_data)
{
	GIOChannel *channel = user_data;
	ssize_t written;
	int fd;

	fd = g_io_channel_unix_get_fd(channel);

	written = write(fd, data, len);
	if (written < 0)
		return;
}

static gboolean receive_bthost(GIOChannel *channel, GIOCondition condition,
							gpointer user_data)
{
	struct bthost *bthost = user_data;
	unsigned char buf[4096];
	ssize_t len;
	int fd;

	if (condition & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));
	if (len < 0)
		return FALSE;

	bthost_receive_h4(bthost, buf, len);

	return TRUE;
}

static guint create_source_bthost(int fd, struct bthost *bthost)
{
	GIOChannel *channel;
	guint source;

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	bthost_set_send_handler(bthost, write_callback, channel);

	source = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				receive_bthost, bthost, NULL);

	g_io_channel_unref(channel);

	return source;
}

static gboolean receive_btdev(GIOChannel *channel, GIOCondition condition,
							gpointer user_data)
{
	struct btdev *btdev = user_data;
	unsigned char buf[4096];
	ssize_t len;
	int fd;

	if (condition & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));
	if (len < 0)
		return FALSE;

	btdev_receive_h4(btdev, buf, len);

	return TRUE;
}

static guint create_source_btdev(int fd, struct btdev *btdev)
{
	GIOChannel *channel;
	guint source;

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	btdev_set_send_handler(btdev, write_callback, channel);

	source = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				receive_btdev, btdev, NULL);

	g_io_channel_unref(channel);

	return source;
}

static bool create_vhci(struct hciemu *hciemu)
{
	struct btdev *btdev;
	uint8_t bdaddr[6];
	const char *str;
	int fd, i;

	btdev = btdev_create(hciemu->btdev_type, 0x00);
	if (!btdev)
		return false;

	str = hciemu_get_address(hciemu);

	for (i = 5; i >= 0; i--, str += 3)
		bdaddr[i] = strtol(str, NULL, 16);

	btdev_set_bdaddr(btdev, bdaddr);
	btdev_set_command_handler(btdev, master_command_callback, hciemu);

	fd = open("/dev/vhci", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		btdev_destroy(btdev);
		return false;
	}

	hciemu->master_dev = btdev;

	hciemu->master_source = create_source_btdev(fd, btdev);

	return true;
}

static bool create_stack(struct hciemu *hciemu)
{
	struct btdev *btdev;
	struct bthost *bthost;
	int sv[2];

	btdev = btdev_create(hciemu->btdev_type, 0x00);
	if (!btdev)
		return false;

	bthost = bthost_create();
	if (!bthost) {
		btdev_destroy(btdev);
		return false;
	}

	btdev_set_command_handler(btdev, client_command_callback, hciemu);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
								0, sv) < 0) {
		bthost_destroy(bthost);
		btdev_destroy(btdev);
		return false;
	}

	hciemu->client_dev = btdev;
	hciemu->host_stack = bthost;

	hciemu->client_source = create_source_btdev(sv[0], btdev);
	hciemu->host_source = create_source_bthost(sv[1], bthost);

	return true;
}

static gboolean start_stack(gpointer user_data)
{
	struct hciemu *hciemu = user_data;

	bthost_start(hciemu->host_stack);

	return FALSE;
}

struct hciemu *hciemu_new(enum hciemu_type type)
{
	struct hciemu *hciemu;

	hciemu = g_try_new0(struct hciemu, 1);
	if (!hciemu)
		return NULL;

	switch (type) {
	case HCIEMU_TYPE_BREDRLE:
		hciemu->btdev_type = BTDEV_TYPE_BREDRLE;
		break;
	case HCIEMU_TYPE_BREDR:
		hciemu->btdev_type = BTDEV_TYPE_BREDR;
		break;
	case HCIEMU_TYPE_LE:
		hciemu->btdev_type = BTDEV_TYPE_LE;
		break;
	default:
		return NULL;
	}

	if (!create_vhci(hciemu)) {
		g_free(hciemu);
		return NULL;
	}

	if (!create_stack(hciemu)) {
		g_source_remove(hciemu->master_source);
		btdev_destroy(hciemu->master_dev);
		g_free(hciemu);
		return NULL;
	}

	g_idle_add(start_stack, hciemu);

	return hciemu_ref(hciemu);
}

struct hciemu *hciemu_ref(struct hciemu *hciemu)
{
	if (!hciemu)
		return NULL;

	__sync_fetch_and_add(&hciemu->ref_count, 1);

	return hciemu;
}

void hciemu_unref(struct hciemu *hciemu)
{
	if (!hciemu)
		return;

	if (__sync_sub_and_fetch(&hciemu->ref_count, 1) > 0)
		return;

	g_list_foreach(hciemu->post_command_hooks, destroy_command_hook, NULL);
	g_list_free(hciemu->post_command_hooks);

	bthost_stop(hciemu->host_stack);

	g_source_remove(hciemu->host_source);
	g_source_remove(hciemu->client_source);
	g_source_remove(hciemu->master_source);

	bthost_destroy(hciemu->host_stack);
	btdev_destroy(hciemu->client_dev);
	btdev_destroy(hciemu->master_dev);

	g_free(hciemu);
}

const char *hciemu_get_address(struct hciemu *hciemu)
{
	if (!hciemu)
		return NULL;

	return "00:FA:CE:1E:55:00";
}

bool hciemu_add_master_post_command_hook(struct hciemu *hciemu,
			hciemu_command_func_t function, void *user_data)
{
	struct hciemu_command_hook *hook;

	if (!hciemu)
		return false;

	hook = g_try_new0(struct hciemu_command_hook, 1);
	if (!hook)
		return false;

	hook->function = function;
	hook->user_data = user_data;

	hciemu->post_command_hooks = g_list_append(hciemu->post_command_hooks,
									hook);

	return true;
}
