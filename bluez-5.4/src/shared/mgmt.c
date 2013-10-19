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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"
#include "lib/hci.h"

#include "src/shared/util.h"
#include "src/shared/mgmt.h"

struct mgmt {
	int ref_count;
	int fd;
	bool close_on_unref;
	GIOChannel *io;
	guint read_watch;
	guint write_watch;
	GQueue *request_queue;
	GQueue *reply_queue;
	GList *pending_list;
	GList *notify_list;
	GList *notify_destroyed;
	unsigned int next_request_id;
	unsigned int next_notify_id;
	bool in_notify;
	bool destroyed;
	void *buf;
	uint16_t len;
	mgmt_debug_func_t debug_callback;
	mgmt_destroy_func_t debug_destroy;
	void *debug_data;
};

struct mgmt_request {
	unsigned int id;
	uint16_t opcode;
	uint16_t index;
	void *buf;
	uint16_t len;
	mgmt_request_func_t callback;
	mgmt_destroy_func_t destroy;
	void *user_data;
};

struct mgmt_notify {
	unsigned int id;
	uint16_t event;
	uint16_t index;
	bool destroyed;
	mgmt_notify_func_t callback;
	mgmt_destroy_func_t destroy;
	void *user_data;
};

static void destroy_request(gpointer data, gpointer user_data)
{
	struct mgmt_request *request = data;

	if (request->destroy)
		request->destroy(request->user_data);

	g_free(request->buf);
	g_free(request);
}

static gint compare_request_id(gconstpointer a, gconstpointer b)
{
	const struct mgmt_request *request = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	return request->id - id;
}

static void destroy_notify(gpointer data, gpointer user_data)
{
	struct mgmt_notify *notify = data;

	if (notify->destroy)
		notify->destroy(notify->user_data);

	g_free(notify);
}

static gint compare_notify_id(gconstpointer a, gconstpointer b)
{
	const struct mgmt_notify *notify = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	return notify->id - id;
}

static void write_watch_destroy(gpointer user_data)
{
	struct mgmt *mgmt = user_data;

	mgmt->write_watch = 0;
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct mgmt *mgmt = user_data;
	struct mgmt_request *request;
	ssize_t bytes_written;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
		return FALSE;

	request = g_queue_pop_head(mgmt->reply_queue);
	if (!request) {
		/* only reply commands can jump the queue */
		if (mgmt->pending_list)
			return FALSE;

		request = g_queue_pop_head(mgmt->request_queue);
		if (!request)
			return FALSE;
	}

	bytes_written = write(mgmt->fd, request->buf, request->len);
	if (bytes_written < 0) {
		util_debug(mgmt->debug_callback, mgmt->debug_data,
				"write failed: %s", strerror(errno));
		if (request->callback)
			request->callback(MGMT_STATUS_FAILED, 0, NULL,
							request->user_data);
		destroy_request(request, NULL);
		return TRUE;
	}

	util_debug(mgmt->debug_callback, mgmt->debug_data,
				"[0x%04x] command 0x%04x",
				request->index, request->opcode);

	util_hexdump('<', request->buf, bytes_written,
				mgmt->debug_callback, mgmt->debug_data);

	mgmt->pending_list = g_list_append(mgmt->pending_list, request);

	return FALSE;
}

static void wakeup_writer(struct mgmt *mgmt)
{
	if (mgmt->pending_list) {
		/* only queued reply commands trigger wakeup */
		if (g_queue_get_length(mgmt->reply_queue) == 0)
			return;
	}

	if (mgmt->write_watch > 0)
		return;

	mgmt->write_watch = g_io_add_watch_full(mgmt->io, G_PRIORITY_HIGH,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, mgmt, write_watch_destroy);
}

static GList *lookup_pending(struct mgmt *mgmt, uint16_t opcode, uint16_t index)
{
	GList *list;

	for (list = g_list_first(mgmt->pending_list); list;
						list = g_list_next(list)) {
		struct mgmt_request *request = list->data;

		if (request->opcode == opcode && request->index == index)
			return list;
	}

	return NULL;
}

static void request_complete(struct mgmt *mgmt, uint8_t status,
					uint16_t opcode, uint16_t index,
					uint16_t length, const void *param)
{
	struct mgmt_request *request;
	GList *list;

	list = lookup_pending(mgmt, opcode, index);
	if (!list)
		return;

	request = list->data;

	mgmt->pending_list = g_list_delete_link(mgmt->pending_list, list);

	if (request->callback)
		request->callback(status, length, param, request->user_data);

	destroy_request(request, NULL);

	if (mgmt->destroyed)
		return;

	wakeup_writer(mgmt);
}

static void process_notify(struct mgmt *mgmt, uint16_t event, uint16_t index,
					uint16_t length, const void *param)
{
	GList *list;

	mgmt->in_notify = true;

	for (list = g_list_first(mgmt->notify_list); list;
						list = g_list_next(list)) {
		struct mgmt_notify *notify = list->data;

		if (notify->destroyed)
			continue;

		if (notify->event != event)
			continue;

		if (notify->index != index && notify->index != MGMT_INDEX_NONE)
			continue;

		if (notify->callback)
			notify->callback(index, length, param,
							notify->user_data);

		if (mgmt->destroyed)
			break;
	}

	mgmt->in_notify = false;

	g_list_foreach(mgmt->notify_destroyed, destroy_notify, NULL);
	g_list_free(mgmt->notify_destroyed);

	mgmt->notify_destroyed = NULL;
}

static void read_watch_destroy(gpointer user_data)
{
	struct mgmt *mgmt = user_data;

	if (mgmt->destroyed) {
		g_free(mgmt);
		return;
	}

	mgmt->read_watch = 0;
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct mgmt *mgmt = user_data;
	struct mgmt_hdr *hdr;
	struct mgmt_ev_cmd_complete *cc;
	struct mgmt_ev_cmd_status *cs;
	ssize_t bytes_read;
	uint16_t opcode, event, index, length;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
		return FALSE;

	bytes_read = read(mgmt->fd, mgmt->buf, mgmt->len);
	if (bytes_read < 0)
		return TRUE;

	util_hexdump('>', mgmt->buf, bytes_read,
				mgmt->debug_callback, mgmt->debug_data);

	if (bytes_read < MGMT_HDR_SIZE)
		return TRUE;

	hdr = mgmt->buf;
	event = btohs(hdr->opcode);
	index = btohs(hdr->index);
	length = btohs(hdr->len);

	if (bytes_read < length + MGMT_HDR_SIZE)
		return TRUE;

	switch (event) {
	case MGMT_EV_CMD_COMPLETE:
		cc = mgmt->buf + MGMT_HDR_SIZE;
		opcode = btohs(cc->opcode);

		util_debug(mgmt->debug_callback, mgmt->debug_data,
				"[0x%04x] command 0x%04x complete: 0x%02x",
						index, opcode, cc->status);

		request_complete(mgmt, cc->status, opcode, index, length - 3,
						mgmt->buf + MGMT_HDR_SIZE + 3);
		break;
	case MGMT_EV_CMD_STATUS:
		cs = mgmt->buf + MGMT_HDR_SIZE;
		opcode = btohs(cs->opcode);

		util_debug(mgmt->debug_callback, mgmt->debug_data,
				"[0x%04x] command 0x%02x status: 0x%02x",
						index, opcode, cs->status);

		request_complete(mgmt, cs->status, opcode, index, 0, NULL);
		break;
	default:
		util_debug(mgmt->debug_callback, mgmt->debug_data,
				"[0x%04x] event 0x%04x", index, event);

		process_notify(mgmt, event, index, length,
						mgmt->buf + MGMT_HDR_SIZE);
		break;
	}

	if (mgmt->destroyed)
		return FALSE;

	return TRUE;
}

struct mgmt *mgmt_new(int fd)
{
	struct mgmt *mgmt;

	if (fd < 0)
		return NULL;

	mgmt = g_try_new0(struct mgmt, 1);
	if (!mgmt)
		return NULL;

	mgmt->fd = fd;
	mgmt->close_on_unref = false;

	mgmt->len = 512;
	mgmt->buf = g_try_malloc(mgmt->len);
	if (!mgmt->buf) {
		g_free(mgmt);
		return NULL;
	}

	mgmt->io = g_io_channel_unix_new(mgmt->fd);

	g_io_channel_set_encoding(mgmt->io, NULL, NULL);
	g_io_channel_set_buffered(mgmt->io, FALSE);

	mgmt->request_queue = g_queue_new();
	mgmt->reply_queue = g_queue_new();

	mgmt->read_watch = g_io_add_watch_full(mgmt->io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, mgmt, read_watch_destroy);

	return mgmt_ref(mgmt);
}

struct mgmt *mgmt_new_default(void)
{
	struct mgmt *mgmt;
	struct sockaddr_hci addr;
	int fd;

	fd = socket(PF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
								BTPROTO_HCI);
	if (fd < 0)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_CONTROL;

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	mgmt = mgmt_new(fd);
	if (!mgmt) {
		close(fd);
		return NULL;
	}

	mgmt->close_on_unref = true;

	return mgmt;
}

struct mgmt *mgmt_ref(struct mgmt *mgmt)
{
	if (!mgmt)
		return NULL;

	__sync_fetch_and_add(&mgmt->ref_count, 1);

	return mgmt;
}

void mgmt_unref(struct mgmt *mgmt)
{
	if (!mgmt)
		return;

	if (__sync_sub_and_fetch(&mgmt->ref_count, 1))
		return;

	mgmt_unregister_all(mgmt);
	mgmt_cancel_all(mgmt);

	g_queue_free(mgmt->reply_queue);
	g_queue_free(mgmt->request_queue);

	if (mgmt->write_watch > 0)
		g_source_remove(mgmt->write_watch);

	if (mgmt->read_watch > 0)
		g_source_remove(mgmt->read_watch);

	g_io_channel_unref(mgmt->io);
	mgmt->io = NULL;

	if (mgmt->close_on_unref)
		close(mgmt->fd);

	if (mgmt->debug_destroy)
		mgmt->debug_destroy(mgmt->debug_data);

	g_free(mgmt->buf);
	mgmt->buf = NULL;

	if (!mgmt->in_notify) {
		g_free(mgmt);
		return;
	}

	mgmt->destroyed = true;
}

bool mgmt_set_debug(struct mgmt *mgmt, mgmt_debug_func_t callback,
				void *user_data, mgmt_destroy_func_t destroy)
{
	if (!mgmt)
		return false;

	if (mgmt->debug_destroy)
		mgmt->debug_destroy(mgmt->debug_data);

	mgmt->debug_callback = callback;
	mgmt->debug_destroy = destroy;
	mgmt->debug_data = user_data;

	return true;
}

bool mgmt_set_close_on_unref(struct mgmt *mgmt, bool do_close)
{
	if (!mgmt)
		return false;

	mgmt->close_on_unref = do_close;

	return true;
}

static struct mgmt_request *create_request(uint16_t opcode, uint16_t index,
				uint16_t length, const void *param,
				mgmt_request_func_t callback,
				void *user_data, mgmt_destroy_func_t destroy)
{
	struct mgmt_request *request;
	struct mgmt_hdr *hdr;

	if (!opcode)
		return NULL;

	if (length > 0 && !param)
		return NULL;

	request = g_try_new0(struct mgmt_request, 1);
	if (!request)
		return NULL;

	request->len = length + MGMT_HDR_SIZE;
	request->buf = g_try_malloc(request->len);
	if (!request->buf) {
		g_free(request);
		return NULL;
	}

	if (length > 0)
		memcpy(request->buf + MGMT_HDR_SIZE, param, length);

	hdr = request->buf;
	hdr->opcode = htobs(opcode);
	hdr->index = htobs(index);
	hdr->len = htobs(length);

	request->opcode = opcode;
	request->index = index;

	request->callback = callback;
	request->destroy = destroy;
	request->user_data = user_data;

	return request;
}

unsigned int mgmt_send(struct mgmt *mgmt, uint16_t opcode, uint16_t index,
				uint16_t length, const void *param,
				mgmt_request_func_t callback,
				void *user_data, mgmt_destroy_func_t destroy)
{
	struct mgmt_request *request;

	if (!mgmt)
		return 0;

	request = create_request(opcode, index, length, param,
					callback, user_data, destroy);
	if (!request)
		return 0;

	if (mgmt->next_request_id < 1)
		mgmt->next_request_id = 1;

	request->id = mgmt->next_request_id++;

	g_queue_push_tail(mgmt->request_queue, request);

	wakeup_writer(mgmt);

	return request->id;
}

unsigned int mgmt_reply(struct mgmt *mgmt, uint16_t opcode, uint16_t index,
				uint16_t length, const void *param,
				mgmt_request_func_t callback,
				void *user_data, mgmt_destroy_func_t destroy)
{
	struct mgmt_request *request;

	if (!mgmt)
		return 0;

	request = create_request(opcode, index, length, param,
					callback, user_data, destroy);
	if (!request)
		return 0;

	if (mgmt->next_request_id < 1)
		mgmt->next_request_id = 1;

	request->id = mgmt->next_request_id++;

	g_queue_push_tail(mgmt->reply_queue, request);

	wakeup_writer(mgmt);

	return request->id;
}

bool mgmt_cancel(struct mgmt *mgmt, unsigned int id)
{
	struct mgmt_request *request;
	GList *list;

	if (!mgmt || !id)
		return false;

	list = g_queue_find_custom(mgmt->request_queue, GUINT_TO_POINTER(id),
							compare_request_id);
	if (list) {
		request = list->data;
		g_queue_delete_link(mgmt->request_queue, list);
		goto done;
	}

	list = g_queue_find_custom(mgmt->reply_queue, GUINT_TO_POINTER(id),
							compare_request_id);
	if (list) {
		request = list->data;
		g_queue_delete_link(mgmt->reply_queue, list);
		goto done;
	}

	list = g_list_find_custom(mgmt->pending_list, GUINT_TO_POINTER(id),
							compare_request_id);
	if (!list)
		return false;

	request = list->data;

	mgmt->pending_list = g_list_delete_link(mgmt->pending_list, list);

done:
	destroy_request(request, NULL);

	wakeup_writer(mgmt);

	return true;
}

bool mgmt_cancel_index(struct mgmt *mgmt, uint16_t index)
{
	GList *list, *next;

	if (!mgmt)
		return false;

	for (list = g_queue_peek_head_link(mgmt->request_queue); list;
								list = next) {
		struct mgmt_request *request = list->data;

		next = g_list_next(list);

		if (request->index != index)
			continue;

		g_queue_delete_link(mgmt->request_queue, list);

		destroy_request(request, NULL);
	}

	for (list = g_queue_peek_head_link(mgmt->reply_queue); list;
								list = next) {
		struct mgmt_request *request = list->data;

		next = g_list_next(list);

		if (request->index != index)
			continue;

		g_queue_delete_link(mgmt->reply_queue, list);

		destroy_request(request, NULL);
	}

	for (list = g_list_first(mgmt->pending_list); list; list = next) {
		struct mgmt_request *request = list->data;

		next = g_list_next(list);

		if (request->index != index)
			continue;

		mgmt->pending_list = g_list_delete_link(mgmt->pending_list,
									list);

		destroy_request(request, NULL);
	}

	return true;
}

bool mgmt_cancel_all(struct mgmt *mgmt)
{
	if (!mgmt)
		return false;

	g_list_foreach(mgmt->pending_list, destroy_request, NULL);
	g_list_free(mgmt->pending_list);
	mgmt->pending_list = NULL;

	g_queue_foreach(mgmt->reply_queue, destroy_request, NULL);
	g_queue_clear(mgmt->reply_queue);

	g_queue_foreach(mgmt->request_queue, destroy_request, NULL);
	g_queue_clear(mgmt->request_queue);

	return true;
}

unsigned int mgmt_register(struct mgmt *mgmt, uint16_t event, uint16_t index,
				mgmt_notify_func_t callback,
				void *user_data, mgmt_destroy_func_t destroy)
{
	struct mgmt_notify *notify;

	if (!mgmt || !event)
		return 0;

	notify = g_try_new0(struct mgmt_notify, 1);
	if (!notify)
		return 0;

	notify->event = event;
	notify->index = index;

	notify->callback = callback;
	notify->destroy = destroy;
	notify->user_data = user_data;

	if (mgmt->next_notify_id < 1)
		mgmt->next_notify_id = 1;

	notify->id = mgmt->next_notify_id++;

	mgmt->notify_list = g_list_append(mgmt->notify_list, notify);

	return notify->id;
}

bool mgmt_unregister(struct mgmt *mgmt, unsigned int id)
{
	struct mgmt_notify *notify;
	GList *list;

	if (!mgmt || !id)
		return false;

	list = g_list_find_custom(mgmt->notify_list,
				GUINT_TO_POINTER(id), compare_notify_id);
	if (!list)
		return false;

	notify = list->data;

	mgmt->notify_list = g_list_remove_link(mgmt->notify_list, list);

	if (!mgmt->in_notify) {
		g_list_free_1(list);
		destroy_notify(notify, NULL);
		return true;
	}

	notify->destroyed = true;

	mgmt->notify_destroyed = g_list_concat(mgmt->notify_destroyed, list);

	return true;
}

bool mgmt_unregister_index(struct mgmt *mgmt, uint16_t index)
{
	GList *list, *next;

	if (!mgmt)
		return false;

	for (list = g_list_first(mgmt->notify_list); list; list = next) {
		struct mgmt_notify *notify = list->data;

		next = g_list_next(list);

		if (notify->index != index)
			continue;

		mgmt->notify_list = g_list_remove_link(mgmt->notify_list, list);

		if (!mgmt->in_notify) {
			g_list_free_1(list);
			destroy_notify(notify, NULL);
			continue;
		}

		notify->destroyed = true;

		mgmt->notify_destroyed = g_list_concat(mgmt->notify_destroyed,
									list);
	}

	return true;
}

static void mark_notify(gpointer data, gpointer user_data)
{
	struct mgmt_notify *notify = data;

	notify->destroyed = true;
}

bool mgmt_unregister_all(struct mgmt *mgmt)
{
	if (!mgmt)
		return false;

	if (!mgmt->in_notify) {
		g_list_foreach(mgmt->notify_list, destroy_notify, NULL);
		g_list_free(mgmt->notify_list);
	} else {
		g_list_foreach(mgmt->notify_list, mark_notify, NULL);
		mgmt->notify_destroyed = g_list_concat(mgmt->notify_destroyed,
							mgmt->notify_list);
	}

	mgmt->notify_list = NULL;

	return true;
}
