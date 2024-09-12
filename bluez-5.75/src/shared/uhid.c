// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "src/shared/io.h"
#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "src/shared/uhid.h"

#define UHID_DEVICE_FILE "/dev/uhid"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

struct uhid_replay {
	bool active;
	struct queue *out;
	struct queue *in;
	struct queue *rout;
	struct queue *rin;
};

struct bt_uhid {
	int ref_count;
	struct io *io;
	unsigned int notify_id;
	struct queue *notify_list;
	struct queue *input;
	bool created;
	bool started;
	struct uhid_replay *replay;
};

struct uhid_notify {
	unsigned int id;
	uint32_t event;
	bt_uhid_callback_t func;
	void *user_data;
};

static void uhid_replay_free(struct uhid_replay *replay)
{
	if (!replay)
		return;

	queue_destroy(replay->rin, NULL);
	queue_destroy(replay->in, free);
	queue_destroy(replay->rout, NULL);
	queue_destroy(replay->out, free);
	free(replay);
}

static void uhid_free(struct bt_uhid *uhid)
{
	if (uhid->io)
		io_destroy(uhid->io);

	if (uhid->notify_list)
		queue_destroy(uhid->notify_list, free);

	if (uhid->input)
		queue_destroy(uhid->input, free);

	uhid_replay_free(uhid->replay);

	free(uhid);
}

static void notify_handler(void *data, void *user_data)
{
	struct uhid_notify *notify = data;
	struct uhid_event *ev = user_data;

	if (notify->event != ev->type)
		return;

	if (notify->func)
		notify->func(ev, notify->user_data);
}

static struct uhid_replay *uhid_replay_new(void)
{
	struct uhid_replay *replay = new0(struct uhid_replay, 1);

	replay->out = queue_new();
	replay->in = queue_new();

	return replay;
}

static int bt_uhid_record(struct bt_uhid *uhid, bool input,
					struct uhid_event *ev)
{
	if (!uhid)
		return -EINVAL;

	/* Capture input events in replay mode and send the next replay event */
	if (uhid->replay && uhid->replay->active && input) {
		queue_pop_head(uhid->replay->rin);
		bt_uhid_replay(uhid);
		return -EALREADY;
	}

	if (!uhid->replay)
		uhid->replay = uhid_replay_new();

	if (input)
		queue_push_tail(uhid->replay->in,
					util_memdup(ev, sizeof(*ev)));
	else
		queue_push_tail(uhid->replay->out,
					util_memdup(ev, sizeof(*ev)));

	return 0;
}

static bool uhid_read_handler(struct io *io, void *user_data)
{
	struct bt_uhid *uhid = user_data;
	int fd;
	ssize_t len;
	struct uhid_event ev;

	fd = io_get_fd(io);
	if (fd < 0)
		return false;

	memset(&ev, 0, sizeof(ev));

	len = read(fd, &ev, sizeof(ev));
	if (len < 0)
		return false;

	if ((size_t) len < sizeof(ev.type))
		return false;

	switch (ev.type) {
	case UHID_GET_REPORT:
	case UHID_SET_REPORT:
		bt_uhid_record(uhid, false, &ev);
		break;
	}

	queue_foreach(uhid->notify_list, notify_handler, &ev);

	return true;
}

struct bt_uhid *bt_uhid_new_default(void)
{
	struct bt_uhid *uhid;
	int fd;

	fd = open(UHID_DEVICE_FILE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	uhid = bt_uhid_new(fd);
	if (!uhid) {
		close(fd);
		return NULL;
	}

	io_set_close_on_destroy(uhid->io, true);

	return uhid;
}

struct bt_uhid *bt_uhid_new(int fd)
{
	struct bt_uhid *uhid;

	uhid = new0(struct bt_uhid, 1);
	uhid->io = io_new(fd);
	if (!uhid->io)
		goto failed;

	uhid->notify_list = queue_new();

	if (!io_set_read_handler(uhid->io, uhid_read_handler, uhid, NULL))
		goto failed;

	return bt_uhid_ref(uhid);

failed:
	uhid_free(uhid);
	return NULL;
}

struct bt_uhid *bt_uhid_ref(struct bt_uhid *uhid)
{
	if (!uhid)
		return NULL;

	__sync_fetch_and_add(&uhid->ref_count, 1);

	return uhid;
}

void bt_uhid_unref(struct bt_uhid *uhid)
{
	if (!uhid)
		return;

	if (__sync_sub_and_fetch(&uhid->ref_count, 1))
		return;

	uhid_free(uhid);
}

bool bt_uhid_set_close_on_unref(struct bt_uhid *uhid, bool do_close)
{
	if (!uhid || !uhid->io)
		return false;

	io_set_close_on_destroy(uhid->io, do_close);

	return true;
}

unsigned int bt_uhid_register(struct bt_uhid *uhid, uint32_t event,
				bt_uhid_callback_t func, void *user_data)
{
	struct uhid_notify *notify;

	if (!uhid)
		return 0;

	notify = new0(struct uhid_notify, 1);
	notify->id = uhid->notify_id++;
	notify->event = event;
	notify->func = func;
	notify->user_data = user_data;

	if (!queue_push_tail(uhid->notify_list, notify)) {
		free(notify);
		return 0;
	}

	return notify->id;
}

static bool match_notify_id(const void *a, const void *b)
{
	const struct uhid_notify *notify = a;
	unsigned int id = PTR_TO_UINT(b);

	return notify->id == id;
}

bool bt_uhid_unregister(struct bt_uhid *uhid, unsigned int id)
{
	struct uhid_notify *notify;

	if (!uhid || !id)
		return false;

	notify = queue_remove_if(uhid->notify_list, match_notify_id,
							UINT_TO_PTR(id));
	if (!notify)
		return false;

	free(notify);
	return true;
}

bool bt_uhid_unregister_all(struct bt_uhid *uhid)
{
	if (!uhid)
		return false;

	queue_remove_all(uhid->notify_list, NULL, NULL, free);
	return true;
}

static int uhid_send(struct bt_uhid *uhid, const struct uhid_event *ev)
{
	ssize_t len;
	struct iovec iov;

	iov.iov_base = (void *) ev;
	iov.iov_len = sizeof(*ev);

	len = io_send(uhid->io, &iov, 1);
	if (len < 0)
		return -errno;

	/* uHID kernel driver does not handle partial writes */
	return len != sizeof(*ev) ? -EIO : 0;
}

int bt_uhid_send(struct bt_uhid *uhid, const struct uhid_event *ev)
{
	if (!uhid || !ev)
		return -EINVAL;

	if (!uhid->io)
		return -ENOTCONN;

	return uhid_send(uhid, ev);
}

static bool input_dequeue(const void *data, const void *match_data)
{
	struct uhid_event *ev = (void *)data;
	struct bt_uhid *uhid = (void *)match_data;

	return bt_uhid_send(uhid, ev) == 0;
}

static void uhid_start(struct uhid_event *ev, void *user_data)
{
	struct bt_uhid *uhid = user_data;

	uhid->started = true;

	/* dequeue input events send while UHID_CREATE2 was in progress */
	queue_remove_all(uhid->input, input_dequeue, uhid, free);
}

int bt_uhid_create(struct bt_uhid *uhid, const char *name, bdaddr_t *src,
			bdaddr_t *dst, uint32_t vendor, uint32_t product,
			uint32_t version, uint32_t country, void *rd_data,
			size_t rd_size)
{
	struct uhid_event ev;
	int err;

	if (!uhid || !name || rd_size > sizeof(ev.u.create2.rd_data))
		return -EINVAL;

	if (uhid->created)
		return 0;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	strncpy((char *) ev.u.create2.name, name,
			sizeof(ev.u.create2.name) - 1);
	if (src)
		sprintf((char *)ev.u.create2.phys,
			"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
			src->b[5], src->b[4], src->b[3], src->b[2], src->b[1],
			src->b[0]);
	if (dst)
		sprintf((char *)ev.u.create2.uniq,
			"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
			dst->b[5], dst->b[4], dst->b[3], dst->b[2], dst->b[1],
			dst->b[0]);
	ev.u.create2.vendor = vendor;
	ev.u.create2.product = product;
	ev.u.create2.version = version;
	ev.u.create2.country = country;
	ev.u.create2.bus = BUS_BLUETOOTH;
	if (rd_size)
		memcpy(ev.u.create2.rd_data, rd_data, rd_size);
	ev.u.create2.rd_size = rd_size;

	err = bt_uhid_send(uhid, &ev);
	if (err)
		return err;

	bt_uhid_register(uhid, UHID_START, uhid_start, uhid);

	uhid->created = true;
	uhid->started = false;

	return 0;
}

bool bt_uhid_created(struct bt_uhid *uhid)
{
	if (!uhid)
		return false;

	return uhid->created;
}

bool bt_uhid_started(struct bt_uhid *uhid)
{
	if (!uhid)
		return false;

	return uhid->started;
}

int bt_uhid_input(struct bt_uhid *uhid, uint8_t number, const void *data,
			size_t size)
{
	struct uhid_event ev;
	struct uhid_input2_req *req = &ev.u.input2;
	size_t len = 0;

	if (!uhid)
		return -EINVAL;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_INPUT2;

	if (number) {
		req->data[len++] = number;
		req->size = 1 + MIN(size, sizeof(req->data) - 1);
	} else
		req->size = MIN(size, sizeof(req->data));

	if (data && size)
		memcpy(&req->data[len], data, req->size - len);

	/* Queue events if UHID_START has not been received yet */
	if (!uhid->started) {
		if (!uhid->input)
			uhid->input = queue_new();

		queue_push_tail(uhid->input, util_memdup(&ev, sizeof(ev)));
		return 0;
	}

	return bt_uhid_send(uhid, &ev);
}

int bt_uhid_set_report_reply(struct bt_uhid *uhid, uint8_t id, uint8_t status)
{
	struct uhid_event ev;
	struct uhid_set_report_reply_req *rsp = &ev.u.set_report_reply;

	if (!uhid)
		return false;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_SET_REPORT_REPLY;
	rsp->id = id;
	rsp->err = status;

	if (bt_uhid_record(uhid, true, &ev) == -EALREADY)
		return 0;

	return bt_uhid_send(uhid, &ev);
}

int bt_uhid_get_report_reply(struct bt_uhid *uhid, uint8_t id, uint8_t number,
				uint8_t status, const void *data, size_t size)
{
	struct uhid_event ev;
	struct uhid_get_report_reply_req *rsp = &ev.u.get_report_reply;
	size_t len = 0;

	if (!uhid)
		return false;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_GET_REPORT_REPLY;
	rsp->id = id;
	rsp->err = status;

	if (!data || !size)
		goto done;

	if (number) {
		rsp->data[len++] = number;
		rsp->size += MIN(size, sizeof(rsp->data) - 1);
	} else
		rsp->size = MIN(size, sizeof(ev.u.input.data));

	memcpy(&rsp->data[len], data, rsp->size - len);

done:
	if (bt_uhid_record(uhid, true, &ev) == -EALREADY)
		return 0;

	return bt_uhid_send(uhid, &ev);
}

int bt_uhid_destroy(struct bt_uhid *uhid)
{
	struct uhid_event ev;
	int err;

	if (!uhid)
		return -EINVAL;

	if (!uhid->created)
		return 0;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;

	err = bt_uhid_send(uhid, &ev);
	if (err < 0)
		return err;

	uhid->created = false;
	uhid_replay_free(uhid->replay);

	return err;
}

static void queue_append(void *data, void *user_data)
{
	queue_push_tail(user_data, data);
}

static struct queue *queue_dup(struct queue *q)
{
	struct queue *dup;

	if (!q || queue_isempty(q))
		return NULL;

	dup = queue_new();

	queue_foreach(q, queue_append, dup);

	return dup;
}

int bt_uhid_replay(struct bt_uhid *uhid)
{
	struct uhid_event *ev;

	if (!uhid || !uhid->started)
		return -EINVAL;

	if (!uhid->replay)
		return 0;

	if (uhid->replay->active)
		goto resend;

	uhid->replay->active = true;
	queue_destroy(uhid->replay->rin, NULL);
	uhid->replay->rin = queue_dup(uhid->replay->in);

	queue_destroy(uhid->replay->rout, NULL);
	uhid->replay->rout = queue_dup(uhid->replay->out);

resend:
	ev = queue_pop_head(uhid->replay->rout);
	if (!ev) {
		uhid->replay->active = false;
		return 0;
	}

	queue_foreach(uhid->notify_list, notify_handler, ev);

	return 0;
}
