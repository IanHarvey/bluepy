/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <glib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "lib/uuid.h"
#include "att.h"
#include "gattrib.h"
#include "gatt.h"

struct discover_primary {
	GAttrib *attrib;
	bt_uuid_t uuid;
	GSList *primaries;
	gatt_cb_t cb;
	void *user_data;
};

/* Used for the Included Services Discovery (ISD) procedure */
struct included_discovery {
	GAttrib		*attrib;
	int		refs;
	int		err;
	uint16_t	end_handle;
	GSList		*includes;
	gatt_cb_t	cb;
	void		*user_data;
};

struct included_uuid_query {
	struct included_discovery	*isd;
	struct gatt_included		*included;
};

struct discover_char {
	GAttrib *attrib;
	bt_uuid_t *uuid;
	uint16_t end;
	GSList *characteristics;
	gatt_cb_t cb;
	void *user_data;
};

static void discover_primary_free(struct discover_primary *dp)
{
	g_slist_free(dp->primaries);
	g_attrib_unref(dp->attrib);
	g_free(dp);
}

static struct included_discovery *isd_ref(struct included_discovery *isd)
{
	__sync_fetch_and_add(&isd->refs, 1);

	return isd;
}

static void isd_unref(struct included_discovery *isd)
{
	if (__sync_sub_and_fetch(&isd->refs, 1) > 0)
		return;

	if (isd->err)
		isd->cb(NULL, isd->err, isd->user_data);
	else
		isd->cb(isd->includes, isd->err, isd->user_data);

	g_slist_free_full(isd->includes, g_free);
	g_attrib_unref(isd->attrib);
	g_free(isd);
}

static void discover_char_free(struct discover_char *dc)
{
	g_slist_free_full(dc->characteristics, g_free);
	g_attrib_unref(dc->attrib);
	g_free(dc->uuid);
	g_free(dc);
}

static guint16 encode_discover_primary(uint16_t start, uint16_t end,
				bt_uuid_t *uuid, uint8_t *pdu, size_t len)
{
	bt_uuid_t prim;
	guint16 plen;

	bt_uuid16_create(&prim, GATT_PRIM_SVC_UUID);

	if (uuid == NULL) {
		/* Discover all primary services */
		plen = enc_read_by_grp_req(start, end, &prim, pdu, len);
	} else {
		uint16_t u16;
		uint128_t u128;
		const void *value;
		size_t vlen;

		/* Discover primary service by service UUID */

		if (uuid->type == BT_UUID16) {
			u16 = htobs(uuid->value.u16);
			value = &u16;
			vlen = sizeof(u16);
		} else {
			htob128(&uuid->value.u128, &u128);
			value = &u128;
			vlen = sizeof(u128);
		}

		plen = enc_find_by_type_req(start, end, &prim, value, vlen,
								pdu, len);
	}

	return plen;
}

static void primary_by_uuid_cb(guint8 status, const guint8 *ipdu,
					guint16 iplen, gpointer user_data)

{
	struct discover_primary *dp = user_data;
	GSList *ranges, *last;
	struct att_range *range;
	uint8_t *buf;
	guint16 oplen;
	int err = 0;
	size_t buflen;

	if (status) {
		err = status == ATT_ECODE_ATTR_NOT_FOUND ? 0 : status;
		goto done;
	}

	ranges = dec_find_by_type_resp(ipdu, iplen);
	if (ranges == NULL)
		goto done;

	dp->primaries = g_slist_concat(dp->primaries, ranges);

	last = g_slist_last(ranges);
	range = last->data;

	if (range->end == 0xffff)
		goto done;

	buf = g_attrib_get_buffer(dp->attrib, &buflen);
	oplen = encode_discover_primary(range->end + 1, 0xffff, &dp->uuid,
								buf, buflen);

	if (oplen == 0)
		goto done;

	g_attrib_send(dp->attrib, 0, buf, oplen, primary_by_uuid_cb, dp, NULL);
	return;

done:
	dp->cb(dp->primaries, err, dp->user_data);
	discover_primary_free(dp);
}

static void primary_all_cb(guint8 status, const guint8 *ipdu, guint16 iplen,
							gpointer user_data)
{
	struct discover_primary *dp = user_data;
	struct att_data_list *list;
	unsigned int i, err;
	uint16_t start, end;

	if (status) {
		err = status == ATT_ECODE_ATTR_NOT_FOUND ? 0 : status;
		goto done;
	}

	list = dec_read_by_grp_resp(ipdu, iplen);
	if (list == NULL) {
		err = ATT_ECODE_IO;
		goto done;
	}

	for (i = 0, end = 0; i < list->num; i++) {
		const uint8_t *data = list->data[i];
		struct gatt_primary *primary;
		bt_uuid_t uuid;

		start = att_get_u16(&data[0]);
		end = att_get_u16(&data[2]);

		if (list->len == 6) {
			bt_uuid_t uuid16 = att_get_uuid16(&data[4]);
			bt_uuid_to_uuid128(&uuid16, &uuid);
		} else if (list->len == 20) {
			uuid = att_get_uuid128(&data[4]);
		} else {
			/* Skipping invalid data */
			continue;
		}

		primary = g_try_new0(struct gatt_primary, 1);
		if (!primary) {
			att_data_list_free(list);
			err = ATT_ECODE_INSUFF_RESOURCES;
			goto done;
		}
		primary->range.start = start;
		primary->range.end = end;
		bt_uuid_to_string(&uuid, primary->uuid, sizeof(primary->uuid));
		dp->primaries = g_slist_append(dp->primaries, primary);
	}

	att_data_list_free(list);
	err = 0;

	if (end != 0xffff) {
		size_t buflen;
		uint8_t *buf = g_attrib_get_buffer(dp->attrib, &buflen);
		guint16 oplen = encode_discover_primary(end + 1, 0xffff, NULL,
								buf, buflen);

		g_attrib_send(dp->attrib, 0, buf, oplen, primary_all_cb,
								dp, NULL);

		return;
	}

done:
	dp->cb(dp->primaries, err, dp->user_data);
	discover_primary_free(dp);
}

guint gatt_discover_primary(GAttrib *attrib, bt_uuid_t *uuid, gatt_cb_t func,
							gpointer user_data)
{
	struct discover_primary *dp;
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(attrib, &buflen);
	GAttribResultFunc cb;
	guint16 plen;

	plen = encode_discover_primary(0x0001, 0xffff, uuid, buf, buflen);
	if (plen == 0)
		return 0;

	dp = g_try_new0(struct discover_primary, 1);
	if (dp == NULL)
		return 0;

	dp->attrib = g_attrib_ref(attrib);
	dp->cb = func;
	dp->user_data = user_data;

	if (uuid) {
		dp->uuid = *uuid;
		cb = primary_by_uuid_cb;
	} else
		cb = primary_all_cb;

	return g_attrib_send(attrib, 0, buf, plen, cb, dp, NULL);
}

static void resolve_included_uuid_cb(uint8_t status, const uint8_t *pdu,
					uint16_t len, gpointer user_data)
{
	struct included_uuid_query *query = user_data;
	struct included_discovery *isd = query->isd;
	struct gatt_included *incl = query->included;
	unsigned int err = status;
	bt_uuid_t uuid;
	size_t buflen;
	uint8_t *buf;

	if (err)
		goto done;

	buf = g_attrib_get_buffer(isd->attrib, &buflen);
	if (dec_read_resp(pdu, len, buf, buflen) != 16) {
		err = ATT_ECODE_IO;
		goto done;
	}

	uuid = att_get_uuid128(buf);
	bt_uuid_to_string(&uuid, incl->uuid, sizeof(incl->uuid));
	isd->includes = g_slist_append(isd->includes, incl);

done:
	if (err)
		g_free(incl);

	if (isd->err == 0)
		isd->err = err;

	isd_unref(isd);

	g_free(query);
}

static guint resolve_included_uuid(struct included_discovery *isd,
					struct gatt_included *incl)
{
	struct included_uuid_query *query;
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(isd->attrib, &buflen);
	guint16 oplen = enc_read_req(incl->range.start, buf, buflen);

	query = g_new0(struct included_uuid_query, 1);
	query->isd = isd_ref(isd);
	query->included = incl;

	return g_attrib_send(isd->attrib, 0, buf, oplen,
				resolve_included_uuid_cb, query, NULL);
}

static struct gatt_included *included_from_buf(const uint8_t *buf, gsize len)
{
	struct gatt_included *incl = g_new0(struct gatt_included, 1);

	incl->handle = att_get_u16(&buf[0]);
	incl->range.start = att_get_u16(&buf[2]);
	incl->range.end = att_get_u16(&buf[4]);

	if (len == 8) {
		bt_uuid_t uuid128;
		bt_uuid_t uuid16 = att_get_uuid16(&buf[6]);

		bt_uuid_to_uuid128(&uuid16, &uuid128);
		bt_uuid_to_string(&uuid128, incl->uuid, sizeof(incl->uuid));
	}

	return incl;
}

static void find_included_cb(uint8_t status, const uint8_t *pdu, uint16_t len,
							gpointer user_data);

static guint find_included(struct included_discovery *isd, uint16_t start)
{
	bt_uuid_t uuid;
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(isd->attrib, &buflen);
	guint16 oplen;

	bt_uuid16_create(&uuid, GATT_INCLUDE_UUID);
	oplen = enc_read_by_type_req(start, isd->end_handle, &uuid,
							buf, buflen);

	return g_attrib_send(isd->attrib, 0, buf, oplen, find_included_cb,
							isd_ref(isd), NULL);
}

static void find_included_cb(uint8_t status, const uint8_t *pdu, uint16_t len,
							gpointer user_data)
{
	struct included_discovery *isd = user_data;
	uint16_t last_handle = isd->end_handle;
	unsigned int err = status;
	struct att_data_list *list;
	int i;

	if (err == ATT_ECODE_ATTR_NOT_FOUND)
		err = 0;

	if (status)
		goto done;

	list = dec_read_by_type_resp(pdu, len);
	if (list == NULL) {
		err = ATT_ECODE_IO;
		goto done;
	}

	if (list->len != 6 && list->len != 8) {
		err = ATT_ECODE_IO;
		att_data_list_free(list);
		goto done;
	}

	for (i = 0; i < list->num; i++) {
		struct gatt_included *incl;

		incl = included_from_buf(list->data[i], list->len);
		last_handle = incl->handle;

		/* 128 bit UUID, needs resolving */
		if (list->len == 6) {
			resolve_included_uuid(isd, incl);
			continue;
		}

		isd->includes = g_slist_append(isd->includes, incl);
	}

	att_data_list_free(list);

	if (last_handle < isd->end_handle)
		find_included(isd, last_handle + 1);

done:
	if (isd->err == 0)
		isd->err = err;

	isd_unref(isd);
}

unsigned int gatt_find_included(GAttrib *attrib, uint16_t start, uint16_t end,
					gatt_cb_t func, gpointer user_data)
{
	struct included_discovery *isd;

	isd = g_new0(struct included_discovery, 1);
	isd->attrib = g_attrib_ref(attrib);
	isd->end_handle = end;
	isd->cb = func;
	isd->user_data = user_data;

	return find_included(isd, start);
}

static void char_discovered_cb(guint8 status, const guint8 *ipdu, guint16 iplen,
							gpointer user_data)
{
	struct discover_char *dc = user_data;
	struct att_data_list *list;
	unsigned int i, err = ATT_ECODE_ATTR_NOT_FOUND;
	size_t buflen;
	uint8_t *buf;
	guint16 oplen;
	bt_uuid_t uuid;
	uint16_t last = 0;

	if (status) {
		err = status;
		goto done;
	}

	list = dec_read_by_type_resp(ipdu, iplen);
	if (list == NULL) {
		err = ATT_ECODE_IO;
		goto done;
	}

	for (i = 0; i < list->num; i++) {
		uint8_t *value = list->data[i];
		struct gatt_char *chars;
		bt_uuid_t uuid;

		last = att_get_u16(value);

		if (list->len == 7) {
			bt_uuid_t uuid16 = att_get_uuid16(&value[5]);
			bt_uuid_to_uuid128(&uuid16, &uuid);
		} else
			uuid = att_get_uuid128(&value[5]);

		if (dc->uuid && bt_uuid_cmp(dc->uuid, &uuid))
			continue;

		chars = g_try_new0(struct gatt_char, 1);
		if (!chars) {
			err = ATT_ECODE_INSUFF_RESOURCES;
			goto done;
		}

		chars->handle = last;
		chars->properties = value[2];
		chars->value_handle = att_get_u16(&value[3]);
		bt_uuid_to_string(&uuid, chars->uuid, sizeof(chars->uuid));
		dc->characteristics = g_slist_append(dc->characteristics,
									chars);
	}

	att_data_list_free(list);

	if (last != 0 && (last + 1 < dc->end)) {
		buf = g_attrib_get_buffer(dc->attrib, &buflen);

		bt_uuid16_create(&uuid, GATT_CHARAC_UUID);

		oplen = enc_read_by_type_req(last + 1, dc->end, &uuid, buf,
									buflen);

		if (oplen == 0)
			return;

		g_attrib_send(dc->attrib, 0, buf, oplen, char_discovered_cb,
								dc, NULL);

		return;
	}

done:
	err = (dc->characteristics ? 0 : err);

	dc->cb(dc->characteristics, err, dc->user_data);
	discover_char_free(dc);
}

guint gatt_discover_char(GAttrib *attrib, uint16_t start, uint16_t end,
						bt_uuid_t *uuid, gatt_cb_t func,
						gpointer user_data)
{
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(attrib, &buflen);
	struct discover_char *dc;
	bt_uuid_t type_uuid;
	guint16 plen;

	bt_uuid16_create(&type_uuid, GATT_CHARAC_UUID);

	plen = enc_read_by_type_req(start, end, &type_uuid, buf, buflen);
	if (plen == 0)
		return 0;

	dc = g_try_new0(struct discover_char, 1);
	if (dc == NULL)
		return 0;

	dc->attrib = g_attrib_ref(attrib);
	dc->cb = func;
	dc->user_data = user_data;
	dc->end = end;
	dc->uuid = g_memdup(uuid, sizeof(bt_uuid_t));

	return g_attrib_send(attrib, 0, buf, plen, char_discovered_cb,
								dc, NULL);
}

guint gatt_read_char_by_uuid(GAttrib *attrib, uint16_t start, uint16_t end,
					bt_uuid_t *uuid, GAttribResultFunc func,
					gpointer user_data)
{
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(attrib, &buflen);
	guint16 plen;

	plen = enc_read_by_type_req(start, end, uuid, buf, buflen);
	if (plen == 0)
		return 0;

	return g_attrib_send(attrib, 0, buf, plen, func, user_data, NULL);
}

struct read_long_data {
	GAttrib *attrib;
	GAttribResultFunc func;
	gpointer user_data;
	guint8 *buffer;
	guint16 size;
	guint16 handle;
	guint id;
	gint ref;
};

static void read_long_destroy(gpointer user_data)
{
	struct read_long_data *long_read = user_data;

	if (__sync_sub_and_fetch(&long_read->ref, 1) > 0)
		return;

	if (long_read->buffer != NULL)
		g_free(long_read->buffer);

	g_free(long_read);
}

static void read_blob_helper(guint8 status, const guint8 *rpdu, guint16 rlen,
							gpointer user_data)
{
	struct read_long_data *long_read = user_data;
	uint8_t *buf;
	size_t buflen;
	guint8 *tmp;
	guint16 plen;
	guint id;

	if (status != 0 || rlen == 1) {
		status = 0;
		goto done;
	}

	tmp = g_try_realloc(long_read->buffer, long_read->size + rlen - 1);

	if (tmp == NULL) {
		status = ATT_ECODE_INSUFF_RESOURCES;
		goto done;
	}

	memcpy(&tmp[long_read->size], &rpdu[1], rlen - 1);
	long_read->buffer = tmp;
	long_read->size += rlen - 1;

	buf = g_attrib_get_buffer(long_read->attrib, &buflen);
	if (rlen < buflen)
		goto done;

	plen = enc_read_blob_req(long_read->handle, long_read->size - 1,
								buf, buflen);
	id = g_attrib_send(long_read->attrib, long_read->id, buf, plen,
				read_blob_helper, long_read, read_long_destroy);

	if (id != 0) {
		__sync_fetch_and_add(&long_read->ref, 1);
		return;
	}

	status = ATT_ECODE_IO;

done:
	long_read->func(status, long_read->buffer, long_read->size,
							long_read->user_data);
}

static void read_char_helper(guint8 status, const guint8 *rpdu,
					guint16 rlen, gpointer user_data)
{
	struct read_long_data *long_read = user_data;
	size_t buflen;
	uint8_t *buf = g_attrib_get_buffer(long_read->attrib, &buflen);
	guint16 plen;
	guint id;

	if (status != 0 || rlen < buflen)
		goto done;

	long_read->buffer = g_malloc(rlen);
	if (long_read->buffer == NULL) {
		status = ATT_ECODE_INSUFF_RESOURCES;
		goto done;
	}

	memcpy(long_read->buffer, rpdu, rlen);
	long_read->size = rlen;

	plen = enc_read_blob_req(long_read->handle, rlen - 1, buf, buflen);

	id = g_attrib_send(long_read->attrib, long_read->id, buf, plen,
				read_blob_helper, long_read, read_long_destroy);
	if (id != 0) {
		__sync_fetch_and_add(&long_read->ref, 1);
		return;
	}

	status = ATT_ECODE_IO;

done:
	long_read->func(status, rpdu, rlen, long_read->user_data);
}

guint gatt_read_char(GAttrib *attrib, uint16_t handle, GAttribResultFunc func,
							gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;
	guint id;
	struct read_long_data *long_read;

	long_read = g_try_new0(struct read_long_data, 1);

	if (long_read == NULL)
		return 0;

	long_read->attrib = attrib;
	long_read->func = func;
	long_read->user_data = user_data;
	long_read->handle = handle;

	buf = g_attrib_get_buffer(attrib, &buflen);
	plen = enc_read_req(handle, buf, buflen);
	id = g_attrib_send(attrib, 0, buf, plen, read_char_helper,
						long_read, read_long_destroy);
	if (id == 0)
		g_free(long_read);
	else {
		__sync_fetch_and_add(&long_read->ref, 1);
		long_read->id = id;
	}

	return id;
}

struct write_long_data {
	GAttrib *attrib;
	GAttribResultFunc func;
	gpointer user_data;
	guint16 handle;
	uint16_t offset;
	uint8_t *value;
	size_t vlen;
};

static guint execute_write(GAttrib *attrib, uint8_t flags,
				GAttribResultFunc func, gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;

	buf = g_attrib_get_buffer(attrib, &buflen);
	plen = enc_exec_write_req(flags, buf, buflen);
	if (plen == 0)
		return 0;

	return g_attrib_send(attrib, 0, buf, plen, func, user_data, NULL);
}

static guint prepare_write(GAttrib *attrib, uint16_t handle, uint16_t offset,
			uint8_t *value, size_t vlen, GAttribResultFunc func,
			gpointer user_data);

static void prepare_write_cb(guint8 status, const guint8 *rpdu,
					guint16 rlen, gpointer user_data)
{
	struct write_long_data *long_write = user_data;

	if (status != 0) {
		long_write->func(status, rpdu, rlen, long_write->user_data);
		return;
	}

	/* Skip Prepare Write Response PDU header (5 bytes) */
	long_write->offset += rlen - 5;

	if (long_write->offset == long_write->vlen){
		execute_write(long_write->attrib, ATT_WRITE_ALL_PREP_WRITES,
				long_write->func, long_write->user_data);
		g_free(long_write->value);
		g_free(long_write);

		return;
	}

	prepare_write(long_write->attrib, long_write->handle,
		long_write->offset, long_write->value, long_write->vlen,
		long_write->func, long_write);
}

static guint prepare_write(GAttrib *attrib, uint16_t handle, uint16_t offset,
			uint8_t *value, size_t vlen, GAttribResultFunc func,
			gpointer user_data)
{
	guint16 plen;
	size_t buflen;
	uint8_t *buf;

	buf = g_attrib_get_buffer(attrib, &buflen);

	plen = enc_prep_write_req(handle, offset, &value[offset], vlen - offset,
								buf, buflen);
	if (plen == 0)
		return 0;

	return g_attrib_send(attrib, 0, buf, plen, prepare_write_cb,
							user_data, NULL);
}

guint gatt_write_char(GAttrib *attrib, uint16_t handle, uint8_t *value,
			size_t vlen, GAttribResultFunc func, gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;
	struct write_long_data *long_write;

	buf = g_attrib_get_buffer(attrib, &buflen);

	/* Only use Write Request/Command if payload fits on a single transfer,
	 * including 3 bytes for the header. */
	if (vlen <= buflen - 3) {
		if (func)
			plen = enc_write_req(handle, value, vlen, buf,
								buflen);
		else
			plen = enc_write_cmd(handle, value, vlen, buf,
								buflen);

		return g_attrib_send(attrib, 0, buf, plen, func,
							user_data, NULL);
	}

	/* Write Long Characteristic Values */
	long_write = g_try_new0(struct write_long_data, 1);
	if (long_write == NULL)
		return 0;

	long_write->attrib = attrib;
	long_write->func = func;
	long_write->user_data = user_data;
	long_write->handle = handle;
	long_write->value = g_memdup(value, vlen);
	long_write->vlen = vlen;

	return prepare_write(attrib, handle, long_write->offset, value, vlen,
							func, long_write);
}

guint gatt_exchange_mtu(GAttrib *attrib, uint16_t mtu, GAttribResultFunc func,
							gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;

	buf = g_attrib_get_buffer(attrib, &buflen);
	plen = enc_mtu_req(mtu, buf, buflen);
	return g_attrib_send(attrib, 0, buf, plen, func, user_data, NULL);
}

guint gatt_find_info(GAttrib *attrib, uint16_t start, uint16_t end,
				GAttribResultFunc func, gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;

	buf = g_attrib_get_buffer(attrib, &buflen);
	plen = enc_find_info_req(start, end, buf, buflen);
	if (plen == 0)
		return 0;

	return g_attrib_send(attrib, 0, buf, plen, func, user_data, NULL);
}

guint gatt_write_cmd(GAttrib *attrib, uint16_t handle, uint8_t *value, int vlen,
				GDestroyNotify notify, gpointer user_data)
{
	uint8_t *buf;
	size_t buflen;
	guint16 plen;

	buf = g_attrib_get_buffer(attrib, &buflen);
	plen = enc_write_cmd(handle, value, vlen, buf, buflen);
	return g_attrib_send(attrib, 0, buf, plen, NULL, user_data, notify);
}

static sdp_data_t *proto_seq_find(sdp_list_t *proto_list)
{
	sdp_list_t *list;
	uuid_t proto;

	sdp_uuid16_create(&proto, ATT_UUID);

	for (list = proto_list; list; list = list->next) {
		sdp_list_t *p;
		for (p = list->data; p; p = p->next) {
			sdp_data_t *seq = p->data;
			if (seq && seq->dtd == SDP_UUID16 &&
				sdp_uuid16_cmp(&proto, &seq->val.uuid) == 0)
				return seq->next;
		}
	}

	return NULL;
}

static gboolean parse_proto_params(sdp_list_t *proto_list, uint16_t *psm,
						uint16_t *start, uint16_t *end)
{
	sdp_data_t *seq1, *seq2;

	if (psm)
		*psm = sdp_get_proto_port(proto_list, L2CAP_UUID);

	/* Getting start and end handle */
	seq1 = proto_seq_find(proto_list);
	if (!seq1 || seq1->dtd != SDP_UINT16)
		return FALSE;

	seq2 = seq1->next;
	if (!seq2 || seq2->dtd != SDP_UINT16)
		return FALSE;

	if (start)
		*start = seq1->val.uint16;

	if (end)
		*end = seq2->val.uint16;

	return TRUE;
}

gboolean gatt_parse_record(const sdp_record_t *rec,
					uuid_t *prim_uuid, uint16_t *psm,
					uint16_t *start, uint16_t *end)
{
	sdp_list_t *list;
	uuid_t uuid;
	gboolean ret;

	if (sdp_get_service_classes(rec, &list) < 0)
		return FALSE;

	memcpy(&uuid, list->data, sizeof(uuid));
	sdp_list_free(list, free);

	if (sdp_get_access_protos(rec, &list) < 0)
		return FALSE;

	ret = parse_proto_params(list, psm, start, end);

	sdp_list_foreach(list, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(list, NULL);

	/* FIXME: replace by bt_uuid_t after uuid_t/sdp code cleanup */
	if (ret && prim_uuid)
		memcpy(prim_uuid, &uuid, sizeof(uuid_t));

	return ret;
}
