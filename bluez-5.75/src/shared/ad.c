// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2015  Google Inc.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <ctype.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"

#include "src/shared/ad.h"

#include "src/eir.h"
#include "src/shared/queue.h"
#include "src/shared/util.h"

struct bt_ad {
	int ref_count;
	uint8_t max_len;
	char *name;
	uint16_t appearance;
	struct queue *service_uuids;
	struct queue *manufacturer_data;
	struct queue *solicit_uuids;
	struct queue *service_data;
	struct queue *data;
};

struct pattern_match_info {
	struct bt_ad *ad;
	struct bt_ad_pattern *current_pattern;
	struct bt_ad_pattern *matched_pattern;
};

struct bt_ad *bt_ad_new(void)
{
	struct bt_ad *ad;

	ad = new0(struct bt_ad, 1);
	ad->max_len = BT_AD_MAX_DATA_LEN;
	ad->service_uuids = queue_new();
	ad->manufacturer_data = queue_new();
	ad->solicit_uuids = queue_new();
	ad->service_data = queue_new();
	ad->data = queue_new();
	ad->appearance = UINT16_MAX;

	return bt_ad_ref(ad);
}

bool bt_ad_set_max_len(struct bt_ad *ad, uint8_t len)
{
	if (!ad || len < BT_AD_MAX_DATA_LEN)
		return false;

	ad->max_len = len;

	return true;
}

static bool ad_replace_data(struct bt_ad *ad, uint8_t type, const void *data,
							size_t len);

static bool ad_is_type_valid(uint8_t type)
{
	if (type > BT_AD_3D_INFO_DATA && type != BT_AD_MANUFACTURER_DATA)
		return false;
	if (type < BT_AD_FLAGS)
		return false;

	return true;
}

struct bt_ad *bt_ad_new_with_data(size_t len, const uint8_t *data)
{
	struct bt_ad *ad;
	struct iovec iov = {
		.iov_base = (void *)data,
		.iov_len = len,
	};
	uint8_t elen;

	if (data == NULL || !len)
		return NULL;

	ad = bt_ad_new();
	if (!ad)
		return NULL;

	bt_ad_set_max_len(ad, len);

	while (util_iov_pull_u8(&iov, &elen)) {
		uint8_t type;
		void *data;

		if (elen == 0 || elen > iov.iov_len)
			break;

		if (!util_iov_pull_u8(&iov, &type))
			goto failed;

		elen--;

		if (!ad_is_type_valid(type))
			goto failed;

		data = util_iov_pull_mem(&iov, elen);
		if (!data)
			goto failed;

		if (!ad_replace_data(ad, type, data, elen))
			goto failed;
	}

	return ad;

failed:
	bt_ad_unref(ad);
	return NULL;
}

struct bt_ad *bt_ad_ref(struct bt_ad *ad)
{
	if (!ad)
		return NULL;

	ad->ref_count++;
	return ad;
}

static void uuid_destroy(void *data)
{
	struct bt_ad_service_data *uuid_data = data;

	free(uuid_data->data);
	free(uuid_data);
}

static bool uuid_data_match(const void *data, const void *elem)
{
	const struct bt_ad_service_data *uuid_data = elem;
	const bt_uuid_t *uuid = data;

	return !bt_uuid_cmp(&uuid_data->uuid, uuid);
}

static void manuf_destroy(void *data)
{
	struct bt_ad_manufacturer_data *manuf = data;

	free(manuf->data);
	free(manuf);
}

static bool manuf_match(const void *data, const void *elem)
{
	const struct bt_ad_manufacturer_data *manuf = elem;
	uint16_t manuf_id = PTR_TO_UINT(elem);

	return manuf->manufacturer_id == manuf_id;
}

static void data_destroy(void *data)
{
	struct bt_ad_data *ad = data;

	free(ad->data);
	free(ad);
}

void bt_ad_unref(struct bt_ad *ad)
{
	if (!ad)
		return;

	if (__sync_sub_and_fetch(&ad->ref_count, 1))
		return;

	queue_destroy(ad->service_uuids, free);

	queue_destroy(ad->manufacturer_data, manuf_destroy);

	queue_destroy(ad->solicit_uuids, free);

	queue_destroy(ad->service_data, uuid_destroy);

	queue_destroy(ad->data, data_destroy);

	free(ad->name);

	free(ad);
}

static bool data_type_match(const void *data, const void *user_data)
{
	const struct bt_ad_data *a = data;
	const uint8_t type = PTR_TO_UINT(user_data);

	return a->type == type;
}

static bool ad_replace_uuid16(struct bt_ad *ad, struct iovec *iov)
{
	uint16_t value;

	while ((util_iov_pull_le16(iov, &value))) {
		bt_uuid_t uuid;

		if (bt_uuid16_create(&uuid, value))
			return false;

		if (bt_ad_has_service_uuid(ad, &uuid))
			continue;

		if (!bt_ad_add_service_uuid(ad, &uuid))
			return false;
	}

	return true;
}

static bool ad_replace_uuid32(struct bt_ad *ad, struct iovec *iov)
{
	uint32_t value;

	while ((util_iov_pull_le32(iov, &value))) {
		bt_uuid_t uuid;

		if (bt_uuid32_create(&uuid, value))
			return false;

		if (bt_ad_has_service_uuid(ad, &uuid))
			continue;

		if (!bt_ad_add_service_uuid(ad, &uuid))
			return false;
	}

	return true;
}

static bool ad_replace_uuid128(struct bt_ad *ad, struct iovec *iov)
{
	void *data;

	while ((data = util_iov_pull_mem(iov, 16))) {
		uint128_t value;
		bt_uuid_t uuid;

		bswap_128(data, &value);

		if (bt_uuid128_create(&uuid, value))
			return false;

		if (bt_ad_has_service_uuid(ad, &uuid))
			continue;

		if (!bt_ad_add_service_uuid(ad, &uuid))
			return false;
	}

	return true;
}

static bool ad_replace_name(struct bt_ad *ad, struct iovec *iov)
{
	char utf8_name[HCI_MAX_NAME_LENGTH + 2];
	int i;

	memset(utf8_name, 0, sizeof(utf8_name));
	strncpy(utf8_name, (const char *)iov->iov_base, iov->iov_len);

	if (strisutf8(utf8_name, iov->iov_len))
		goto done;

	/* Assume ASCII, and replace all non-ASCII with spaces */
	for (i = 0; utf8_name[i] != '\0'; i++) {
		if (!isascii(utf8_name[i]))
			utf8_name[i] = ' ';
	}

	/* Remove leading and trailing whitespace characters */
	strstrip(utf8_name);

done:
	return bt_ad_add_name(ad, utf8_name);
}

static bool ad_replace_uuid16_data(struct bt_ad *ad, struct iovec *iov)
{
	uint16_t value;
	bt_uuid_t uuid;

	if (!util_iov_pull_le16(iov, &value))
		return false;

	if (bt_uuid16_create(&uuid, value))
		return false;

	return bt_ad_add_service_data(ad, &uuid, iov->iov_base, iov->iov_len);
}

static bool ad_replace_uuid32_data(struct bt_ad *ad, struct iovec *iov)
{
	uint32_t value;
	bt_uuid_t uuid;

	if (!util_iov_pull_le32(iov, &value))
		return false;

	if (bt_uuid32_create(&uuid, value))
		return false;

	return bt_ad_add_service_data(ad, &uuid, iov->iov_base, iov->iov_len);
}

static bool ad_replace_uuid128_data(struct bt_ad *ad, struct iovec *iov)
{
	void *data;
	uint128_t value;
	bt_uuid_t uuid;

	data = util_iov_pull_mem(iov, 16);
	if (!data)
		return false;

	bswap_128(data, &value);

	if (bt_uuid128_create(&uuid, value))
		return false;

	return bt_ad_add_service_data(ad, &uuid, iov->iov_base, iov->iov_len);
}

static bool ad_replace_manufacturer_data(struct bt_ad *ad, struct iovec *iov)
{
	uint16_t value;

	if (!util_iov_pull_le16(iov, &value))
		return false;

	return bt_ad_add_manufacturer_data(ad, value, iov->iov_base,
							iov->iov_len);
}

static bool ad_replace_data(struct bt_ad *ad, uint8_t type, const void *data,
							size_t len)
{
	struct bt_ad_data *new_data;
	struct iovec iov = {
		.iov_base = (void *)data,
		.iov_len = len,
	};

	switch (type) {
	case BT_AD_UUID16_SOME:
	case BT_AD_UUID16_ALL:
		return ad_replace_uuid16(ad, &iov);
	case BT_AD_UUID32_SOME:
	case BT_AD_UUID32_ALL:
		return ad_replace_uuid32(ad, &iov);
	case BT_AD_UUID128_SOME:
	case BT_AD_UUID128_ALL:
		return ad_replace_uuid128(ad, &iov);
	case BT_AD_NAME_SHORT:
	case BT_AD_NAME_COMPLETE:
		return ad_replace_name(ad, &iov);
	case BT_AD_SERVICE_DATA16:
		return ad_replace_uuid16_data(ad, &iov);
	case BT_AD_SERVICE_DATA32:
		return ad_replace_uuid32_data(ad, &iov);
	case BT_AD_SERVICE_DATA128:
		return ad_replace_uuid128_data(ad, &iov);
	case BT_AD_MANUFACTURER_DATA:
		return ad_replace_manufacturer_data(ad, &iov);
	}

	new_data = queue_find(ad->data, data_type_match, UINT_TO_PTR(type));
	if (new_data) {
		if (new_data->len == len && !memcmp(new_data->data, data, len))
			return false;
		new_data->data = realloc(new_data->data, len);
		memcpy(new_data->data, data, len);
		new_data->len = len;
		return true;
	}

	new_data = new0(struct bt_ad_data, 1);
	new_data->type = type;
	new_data->data = util_memdup(data, len);
	if (!new_data->data) {
		free(new_data);
		return false;
	}

	new_data->len = len;

	if (queue_push_tail(ad->data, new_data))
		return true;

	data_destroy(new_data);

	return false;
}

static size_t uuid_list_length(struct queue *uuid_queue)
{
	bool uuid16_included = false;
	bool uuid32_included = false;
	bool uuid128_included = false;
	size_t length = 0;
	const struct queue_entry *entry;

	entry = queue_get_entries(uuid_queue);

	while (entry) {
		bt_uuid_t *uuid = entry->data;

		length += bt_uuid_len(uuid);

		if (uuid->type == BT_UUID16)
			uuid16_included = true;
		else if (uuid->type == BT_UUID32)
			uuid32_included = true;
		else
			uuid128_included = true;

		entry = entry->next;
	}

	if (uuid16_included)
		length += 2;

	if (uuid32_included)
		length += 2;

	if (uuid128_included)
		length += 2;

	return length;
}

static size_t mfg_data_length(struct queue *manuf_data)
{
	size_t length = 0;
	const struct queue_entry *entry;

	entry = queue_get_entries(manuf_data);

	while (entry) {
		struct bt_ad_manufacturer_data *data = entry->data;

		length += 2 + sizeof(uint16_t) + data->len;

		entry = entry->next;
	}

	return length;
}

static size_t uuid_data_length(struct queue *uuid_data)
{
	size_t length = 0;
	const struct queue_entry *entry;

	entry = queue_get_entries(uuid_data);

	while (entry) {
		struct bt_ad_service_data *data = entry->data;

		length += 2 + bt_uuid_len(&data->uuid) + data->len;

		entry = entry->next;
	}

	return length;
}

static size_t name_length(struct bt_ad *ad, size_t *pos)
{
	size_t len;

	if (!ad->name)
		return 0;

	len = 2 + strlen(ad->name);

	if (len > ad->max_len - (*pos))
		len = ad->max_len - (*pos);

	return len;
}

static size_t data_length(struct queue *queue)
{
	size_t length = 0;
	const struct queue_entry *entry;

	entry = queue_get_entries(queue);

	while (entry) {
		struct bt_ad_data *data = entry->data;

		length += 1 + 1 + data->len;

		entry = entry->next;
	}

	return length;
}

static size_t calculate_length(struct bt_ad *ad)
{
	size_t length = 0;

	length += uuid_list_length(ad->service_uuids);

	length += uuid_list_length(ad->solicit_uuids);

	length += mfg_data_length(ad->manufacturer_data);

	length += uuid_data_length(ad->service_data);

	length += name_length(ad, &length);

	length += ad->appearance != UINT16_MAX ? 4 : 0;

	length += data_length(ad->data);

	return length;
}

static void serialize_uuids(struct queue *uuids, uint8_t uuid_type,
				uint8_t ad_type, struct iovec *iov)
{
	const struct queue_entry *entry = queue_get_entries(uuids);
	uint8_t *len = NULL;

	while (entry) {
		bt_uuid_t *uuid = entry->data;

		if (uuid->type == uuid_type) {
			if (!len) {
				len = iov->iov_base + iov->iov_len;
				util_iov_push_u8(iov, 1);
				util_iov_push_u8(iov, ad_type);
			}

			switch (uuid->type) {
			case BT_UUID16:
				util_iov_push_le16(iov, uuid->value.u16);
				*len += 2;
				break;
			case BT_UUID32:
				util_iov_push_le32(iov, uuid->value.u32);
				*len += 4;
				break;
			case BT_UUID128:
				bt_uuid_to_le(uuid, util_iov_push(iov, 16));
				*len += 16;
				break;
			case BT_UUID_UNSPEC:
				break;
			}
		}

		entry = entry->next;
	}
}

static void serialize_service_uuids(struct queue *uuids, struct iovec *iov)
{
	serialize_uuids(uuids, BT_UUID16, BT_AD_UUID16_ALL, iov);

	serialize_uuids(uuids, BT_UUID32, BT_AD_UUID32_ALL, iov);

	serialize_uuids(uuids, BT_UUID128, BT_AD_UUID128_ALL, iov);
}

static void serialize_solicit_uuids(struct queue *uuids, struct iovec *iov)
{
	serialize_uuids(uuids, BT_UUID16, BT_AD_SOLICIT16, iov);

	serialize_uuids(uuids, BT_UUID32, BT_AD_SOLICIT32, iov);

	serialize_uuids(uuids, BT_UUID128, BT_AD_SOLICIT128, iov);
}

static void serialize_manuf_data(struct queue *manuf_data, struct iovec *iov)
{
	const struct queue_entry *entry = queue_get_entries(manuf_data);

	while (entry) {
		struct bt_ad_manufacturer_data *data = entry->data;

		util_iov_push_u8(iov, data->len + 2 + 1);
		util_iov_push_u8(iov, BT_AD_MANUFACTURER_DATA);

		util_iov_push_le16(iov, data->manufacturer_id);
		util_iov_push_mem(iov, data->len, data->data);

		entry = entry->next;
	}
}

static void serialize_service_data(struct queue *service_data,
					struct iovec *iov)
{
	const struct queue_entry *entry = queue_get_entries(service_data);

	while (entry) {
		struct bt_ad_service_data *data = entry->data;
		int uuid_len = bt_uuid_len(&data->uuid);

		util_iov_push_u8(iov, data->len + uuid_len + 1);

		switch (uuid_len) {
		case 2:
			util_iov_push_u8(iov, BT_AD_SERVICE_DATA16);
			util_iov_push_le16(iov, data->uuid.value.u16);
			break;
		case 4:
			util_iov_push_u8(iov, BT_AD_SERVICE_DATA32);
			util_iov_push_le32(iov, data->uuid.value.u32);
			break;
		case 16:
			util_iov_push_u8(iov, BT_AD_SERVICE_DATA128);
			bt_uuid_to_le(&data->uuid,
					util_iov_push(iov, uuid_len));
			break;
		}

		util_iov_push_mem(iov, data->len, data->data);

		entry = entry->next;
	}
}

static void serialize_name(struct bt_ad *ad, struct iovec *iov)
{
	size_t len;
	uint8_t type = BT_AD_NAME_COMPLETE;

	if (!ad->name)
		return;

	len = strlen(ad->name);
	if (len > ad->max_len - (iov->iov_len + 2)) {
		type = BT_AD_NAME_SHORT;
		len = ad->max_len - (iov->iov_len + 2);
	}

	util_iov_push_u8(iov, len + 1);
	util_iov_push_u8(iov, type);
	util_iov_push_mem(iov, len, ad->name);
}

static void serialize_appearance(struct bt_ad *ad, struct iovec *iov)
{
	if (ad->appearance == UINT16_MAX)
		return;

	util_iov_push_u8(iov, sizeof(ad->appearance) + 1);
	util_iov_push_u8(iov, BT_AD_GAP_APPEARANCE);
	util_iov_push_le16(iov, ad->appearance);
}

static void serialize_data(struct queue *queue, struct iovec *iov)
{
	const struct queue_entry *entry = queue_get_entries(queue);

	while (entry) {
		struct bt_ad_data *data = entry->data;

		util_iov_push_u8(iov, data->len + 1);
		util_iov_push_u8(iov, data->type);
		util_iov_push_mem(iov, data->len, data->data);

		entry = entry->next;
	}
}

uint8_t *bt_ad_generate(struct bt_ad *ad, size_t *length)
{
	struct iovec iov;

	if (!ad)
		return NULL;

	*length = calculate_length(ad);

	if (*length > ad->max_len)
		return NULL;

	iov.iov_base = malloc0(*length);
	if (!iov.iov_base)
		return NULL;

	iov.iov_len = 0;

	serialize_service_uuids(ad->service_uuids, &iov);

	serialize_solicit_uuids(ad->solicit_uuids, &iov);

	serialize_manuf_data(ad->manufacturer_data, &iov);

	serialize_service_data(ad->service_data, &iov);

	serialize_name(ad, &iov);

	serialize_appearance(ad, &iov);

	serialize_data(ad->data, &iov);

	return iov.iov_base;
}

bool bt_ad_is_empty(struct bt_ad *ad)
{
	/* If any of the bt_ad fields are non-empty or don't have the default
	 * value, then bt_ad_generate will return a non-empty buffer
	 */
	if (!ad->name &&
		ad->appearance == UINT16_MAX &&
		queue_isempty(ad->service_uuids) &&
		queue_isempty(ad->manufacturer_data) &&
		queue_isempty(ad->solicit_uuids) &&
		queue_isempty(ad->service_data) &&
		queue_isempty(ad->data)) {
		return true;
	}
	return false;
}

static bool queue_add_uuid(struct queue *queue, const bt_uuid_t *uuid)
{
	bt_uuid_t *new_uuid;

	if (!queue)
		return false;

	new_uuid = new0(bt_uuid_t, 1);

	*new_uuid = *uuid;

	if (queue_push_tail(queue, new_uuid))
		return true;

	free(new_uuid);

	return false;
}

static bool uuid_match(const void *data, const void *elem)
{
	const bt_uuid_t *match_uuid = data;
	const bt_uuid_t *uuid = elem;

	return !bt_uuid_cmp(match_uuid, uuid);
}

static bool queue_remove_uuid(struct queue *queue, bt_uuid_t *uuid)
{
	bt_uuid_t *removed;

	if (!queue || !uuid)
		return false;

	removed = queue_remove_if(queue, uuid_match, uuid);

	if (removed) {
		free(removed);
		return true;
	}

	return false;
}

bool bt_ad_add_service_uuid(struct bt_ad *ad, const bt_uuid_t *uuid)
{
	if (!ad)
		return false;

	return queue_add_uuid(ad->service_uuids, uuid);
}

bool bt_ad_has_service_uuid(struct bt_ad *ad, const bt_uuid_t *uuid)
{
	if (!ad)
		return false;

	return queue_find(ad->service_uuids, uuid_match, uuid);
}

bool bt_ad_remove_service_uuid(struct bt_ad *ad, bt_uuid_t *uuid)
{
	if (!ad)
		return false;

	return queue_remove_uuid(ad->service_uuids, uuid);
}

void bt_ad_clear_service_uuid(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->service_uuids, NULL, NULL, free);
}

static bool manufacturer_id_data_match(const void *data, const void *user_data)
{
	const struct bt_ad_manufacturer_data *m = data;
	uint16_t id = PTR_TO_UINT(user_data);

	return m->manufacturer_id == id;
}

bool bt_ad_add_manufacturer_data(struct bt_ad *ad, uint16_t manufacturer_id,
							void *data, size_t len)
{
	struct bt_ad_manufacturer_data *new_data;

	if (!ad)
		return false;

	if (len > (ad->max_len - 2 - sizeof(uint16_t)))
		return false;

	new_data = queue_find(ad->manufacturer_data, manufacturer_id_data_match,
						UINT_TO_PTR(manufacturer_id));
	if (new_data) {
		if (new_data->len == len && !memcmp(new_data->data, data, len))
			return false;
		new_data->data = realloc(new_data->data, len);
		memcpy(new_data->data, data, len);
		new_data->len = len;
		return true;
	}

	new_data = new0(struct bt_ad_manufacturer_data, 1);
	new_data->manufacturer_id = manufacturer_id;

	new_data->data = malloc(len);
	if (!new_data->data) {
		free(new_data);
		return false;
	}

	memcpy(new_data->data, data, len);

	new_data->len = len;

	if (queue_push_tail(ad->manufacturer_data, new_data))
		return true;

	manuf_destroy(new_data);

	return false;
}

static bool manufacturer_data_match(const void *data, const void *user_data)
{
	const struct bt_ad_manufacturer_data *m1 = data;
	const struct bt_ad_manufacturer_data *m2 = user_data;

	if (m1->manufacturer_id != m2->manufacturer_id)
		return false;

	if (m1->len != m2->len)
		return false;

	return !memcmp(m1->data, m2->data, m1->len);
}

bool bt_ad_has_manufacturer_data(struct bt_ad *ad,
				const struct bt_ad_manufacturer_data *data)
{
	if (!ad)
		return false;

	if (!data)
		return !queue_isempty(ad->manufacturer_data);

	return queue_find(ad->manufacturer_data, manufacturer_data_match, data);
}

void bt_ad_foreach_manufacturer_data(struct bt_ad *ad, bt_ad_func_t func,
							void *user_data)
{
	if (!ad)
		return;

	queue_foreach(ad->manufacturer_data, func, user_data);
}

bool bt_ad_remove_manufacturer_data(struct bt_ad *ad, uint16_t manufacturer_id)
{
	struct bt_ad_manufacturer_data *data;

	if (!ad)
		return false;

	data = queue_remove_if(ad->manufacturer_data, manuf_match,
						UINT_TO_PTR(manufacturer_id));

	if (!data)
		return false;

	manuf_destroy(data);

	return true;
}

void bt_ad_clear_manufacturer_data(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->manufacturer_data, NULL, NULL, manuf_destroy);
}

bool bt_ad_add_solicit_uuid(struct bt_ad *ad, const bt_uuid_t *uuid)
{
	if (!ad)
		return false;

	return queue_add_uuid(ad->solicit_uuids, uuid);
}

bool bt_ad_remove_solicit_uuid(struct bt_ad *ad, bt_uuid_t *uuid)
{
	if (!ad)
		return false;

	return queue_remove_uuid(ad->solicit_uuids, uuid);
}

void bt_ad_clear_solicit_uuid(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->solicit_uuids, NULL, NULL, free);
}


static bool service_uuid_match(const void *data, const void *user_data)
{
	const struct bt_ad_service_data *s = data;
	const bt_uuid_t *uuid = user_data;

	return !bt_uuid_cmp(&s->uuid, uuid);
}

bool bt_ad_add_service_data(struct bt_ad *ad, const bt_uuid_t *uuid, void *data,
								size_t len)
{
	struct bt_ad_service_data *new_data;

	if (!ad)
		return false;

	if (len > (ad->max_len - 2 - (size_t)bt_uuid_len(uuid)))
		return false;

	new_data = queue_find(ad->service_data, service_uuid_match, uuid);
	if (new_data) {
		if (new_data->len == len && !memcmp(new_data->data, data, len))
			return false;
		new_data->data = realloc(new_data->data, len);
		memcpy(new_data->data, data, len);
		new_data->len = len;
		return true;
	}

	new_data = new0(struct bt_ad_service_data, 1);

	new_data->uuid = *uuid;

	new_data->data = malloc(len);
	if (!new_data->data) {
		free(new_data);
		return false;
	}

	memcpy(new_data->data, data, len);

	new_data->len = len;

	if (queue_push_tail(ad->service_data, new_data))
		return true;

	uuid_destroy(new_data);

	return false;
}

static bool service_data_match(const void *data, const void *user_data)
{
	const struct bt_ad_service_data *s1 = data;
	const struct bt_ad_service_data *s2 = user_data;

	if (bt_uuid_cmp(&s1->uuid, &s2->uuid))
		return false;

	if (s1->len != s2->len)
		return false;

	return !memcmp(s1->data, s2->data, s1->len);
}

bool bt_ad_has_service_data(struct bt_ad *ad,
					const struct bt_ad_service_data *data)
{
	if (!ad)
		return false;

	if (!data)
		return !queue_isempty(ad->service_data);

	return queue_find(ad->service_data, service_data_match, data);
}

void bt_ad_foreach_service_data(struct bt_ad *ad, bt_ad_func_t func,
							void *user_data)
{
	if (!ad)
		return;

	queue_foreach(ad->service_data, func, user_data);
}

bool bt_ad_remove_service_data(struct bt_ad *ad, bt_uuid_t *uuid)
{
	struct bt_ad_service_data *data;

	if (!ad)
		return false;

	data = queue_remove_if(ad->service_data, uuid_data_match, uuid);

	if (!data)
		return false;

	uuid_destroy(data);

	return true;
}

void bt_ad_clear_service_data(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->service_data, NULL, NULL, uuid_destroy);
}

bool bt_ad_add_name(struct bt_ad *ad, const char *name)
{
	if (!ad)
		return false;

	free(ad->name);

	ad->name = strdup(name);

	return true;
}

const char *bt_ad_get_name(struct bt_ad *ad)
{
	if (!ad)
		return false;

	return ad->name;
}

void bt_ad_clear_name(struct bt_ad *ad)
{
	if (!ad)
		return;

	free(ad->name);
	ad->name = NULL;
}

bool bt_ad_add_appearance(struct bt_ad *ad, uint16_t appearance)
{
	if (!ad)
		return false;

	ad->appearance = appearance;

	return true;
}

void bt_ad_clear_appearance(struct bt_ad *ad)
{
	if (!ad)
		return;

	ad->appearance = UINT16_MAX;
}

bool bt_ad_add_flags(struct bt_ad *ad, uint8_t *flags, size_t len)
{
	if (!ad)
		return false;

	/* TODO: Add table to check other flags */
	if (len > 1 || flags[0] & 0xe0)
		return false;

	return ad_replace_data(ad, BT_AD_FLAGS, flags, len);
}

uint8_t bt_ad_get_flags(struct bt_ad *ad)
{
	struct bt_ad_data *data;

	if (!ad)
		return 0;

	data = queue_find(ad->data, data_type_match, UINT_TO_PTR(BT_AD_FLAGS));
	if (!data || data->len != 1)
		return 0;

	return data->data[0];
}

bool bt_ad_has_flags(struct bt_ad *ad)
{
	struct bt_ad_data *data;

	if (!ad)
		return false;

	data = queue_find(ad->data, data_type_match, UINT_TO_PTR(BT_AD_FLAGS));
	if (!data)
		return false;

	return true;
}

void bt_ad_clear_flags(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->data, data_type_match, UINT_TO_PTR(BT_AD_FLAGS),
							data_destroy);
}

static uint8_t type_reject_list[] = {
	BT_AD_FLAGS,
	BT_AD_UUID16_SOME,
	BT_AD_UUID16_ALL,
	BT_AD_UUID32_SOME,
	BT_AD_UUID32_ALL,
	BT_AD_UUID128_SOME,
	BT_AD_UUID128_ALL,
	BT_AD_NAME_SHORT,
	BT_AD_NAME_COMPLETE,
	BT_AD_TX_POWER,
	BT_AD_CLASS_OF_DEV,
	BT_AD_SSP_HASH,
	BT_AD_SSP_RANDOMIZER,
	BT_AD_DEVICE_ID,
	BT_AD_SMP_TK,
	BT_AD_SMP_OOB_FLAGS,
	BT_AD_PERIPHERAL_CONN_INTERVAL,
	BT_AD_SOLICIT16,
	BT_AD_SOLICIT128,
	BT_AD_SERVICE_DATA16,
	BT_AD_PUBLIC_ADDRESS,
	BT_AD_RANDOM_ADDRESS,
	BT_AD_GAP_APPEARANCE,
	BT_AD_ADVERTISING_INTERVAL,
	BT_AD_LE_DEVICE_ADDRESS,
	BT_AD_LE_ROLE,
	BT_AD_SSP_HASH_P256,
	BT_AD_SSP_RANDOMIZER_P256,
	BT_AD_SOLICIT32,
	BT_AD_SERVICE_DATA32,
	BT_AD_SERVICE_DATA128,
	BT_AD_LE_SC_CONFIRM_VALUE,
	BT_AD_LE_SC_RANDOM_VALUE,
	BT_AD_LE_SUPPORTED_FEATURES,
	BT_AD_CHANNEL_MAP_UPDATE_IND,
	BT_AD_MESH_PROV,
	BT_AD_MESH_DATA,
	BT_AD_MESH_BEACON,
	BT_AD_3D_INFO_DATA,
	BT_AD_MANUFACTURER_DATA,
};

bool bt_ad_add_data(struct bt_ad *ad, uint8_t type, void *data, size_t len)
{
	size_t i;

	if (!ad)
		return false;

	if (len > (size_t)(ad->max_len - 2))
		return false;

	for (i = 0; i < sizeof(type_reject_list); i++) {
		if (type == type_reject_list[i])
			return false;
	}

	return ad_replace_data(ad, type, data, len);
}

static bool data_match(const void *data, const void *user_data)
{
	const struct bt_ad_data *d1 = data;
	const struct bt_ad_data *d2 = user_data;

	if (d1->type != d2->type)
		return false;

	if (!d2->len && !d2->data)
		return true;

	if (d1->len != d2->len)
		return false;

	return !memcmp(d1->data, d2->data, d1->len);
}

bool bt_ad_has_data(struct bt_ad *ad, const struct bt_ad_data *data)
{
	if (!ad)
		return false;

	if (!data)
		return !queue_isempty(ad->data);

	return queue_find(ad->data, data_match, data);
}

void bt_ad_foreach_data(struct bt_ad *ad, bt_ad_func_t func, void *user_data)
{
	if (!ad)
		return;

	queue_foreach(ad->data, func, user_data);
}

bool bt_ad_remove_data(struct bt_ad *ad, uint8_t type)
{
	struct bt_ad_data *data;

	if (!ad)
		return false;

	data = queue_remove_if(ad->data, data_type_match, UINT_TO_PTR(type));
	if (!data)
		return false;

	data_destroy(data);

	return true;
}

void bt_ad_clear_data(struct bt_ad *ad)
{
	if (!ad)
		return;

	queue_remove_all(ad->data, NULL, NULL, data_destroy);
}

int8_t bt_ad_get_tx_power(struct bt_ad *ad)
{
	struct bt_ad_data *data;

	if (!ad)
		return 0;

	data = queue_find(ad->data, data_type_match,
					UINT_TO_PTR(BT_AD_TX_POWER));
	if (!data || data->len != 1)
		return 127;

	return data->data[0];
}

struct bt_ad_pattern *bt_ad_pattern_new(uint8_t type, size_t offset, size_t len,
							const uint8_t *data)
{
	struct bt_ad_pattern *pattern;

	if (!data || !len || offset >= BT_AD_MAX_DATA_LEN ||
		len > BT_AD_MAX_DATA_LEN || offset + len > BT_AD_MAX_DATA_LEN) {
		return NULL;
	}

	if (!ad_is_type_valid(type))
		return NULL;

	pattern = new0(struct bt_ad_pattern, 1);
	if (!pattern)
		return NULL;

	pattern->len = len;
	pattern->type = type;
	pattern->offset = offset;
	memcpy(pattern->data, data, len);

	return pattern;
}

static bool match_manufacturer(const void *data, const void *user_data)
{
	const struct bt_ad_manufacturer_data *manufacturer_data = data;
	const struct pattern_match_info *info = user_data;
	const struct bt_ad_pattern *pattern;
	uint8_t all_data[BT_AD_MAX_DATA_LEN];

	if (!manufacturer_data || !info)
		return false;

	if (info->matched_pattern)
		return false;

	pattern = info->current_pattern;

	if (!pattern || pattern->type != BT_AD_MANUFACTURER_DATA)
		return false;

	/* Take the manufacturer ID into account */
	if (manufacturer_data->len + 2 < pattern->offset + pattern->len)
		return false;

	memcpy(&all_data[0], &manufacturer_data->manufacturer_id, 2);
	memcpy(&all_data[2], manufacturer_data->data, manufacturer_data->len);

	if (!memcmp(all_data + pattern->offset, pattern->data,
							pattern->len)) {
		return true;
	}

	return false;
}

static bool match_service(const void *data, const void *user_data)
{
	const struct bt_ad_service_data *service_data = data;
	const struct pattern_match_info *info = user_data;
	const struct bt_ad_pattern *pattern;

	if (!service_data || !info)
		return false;

	if (info->matched_pattern)
		return false;

	pattern = info->current_pattern;

	if (!pattern)
		return false;

	switch (pattern->type) {
	case BT_AD_SERVICE_DATA16:
	case BT_AD_SERVICE_DATA32:
	case BT_AD_SERVICE_DATA128:
		break;
	default:
		return false;
	}

	if (service_data->len < pattern->offset + pattern->len)
		return false;

	if (!memcmp(service_data->data + pattern->offset, pattern->data,
							pattern->len)) {
		return true;
	}

	return false;
}

static bool match_ad_data(const void *data, const void *user_data)
{
	const struct bt_ad_data *ad_data = data;
	const struct pattern_match_info *info = user_data;
	const struct bt_ad_pattern *pattern;

	if (!ad_data || !info)
		return false;

	if (info->matched_pattern)
		return false;

	pattern = info->current_pattern;

	if (!pattern || ad_data->type != pattern->type)
		return false;

	if (ad_data->len < pattern->offset + pattern->len)
		return false;

	if (!memcmp(ad_data->data + pattern->offset, pattern->data,
								pattern->len)) {
		return true;
	}

	return false;
}

static void pattern_match(void *data, void *user_data)
{
	struct bt_ad_pattern *pattern = data;
	struct pattern_match_info *info = user_data;
	struct bt_ad *ad;
	void *matched = NULL;

	if (!pattern || !info)
		return;

	if (info->matched_pattern)
		return;

	info->current_pattern = pattern;
	ad = info->ad;

	if (!ad)
		return;

	switch (pattern->type) {
	case BT_AD_MANUFACTURER_DATA:
		matched = queue_find(ad->manufacturer_data, match_manufacturer,
				user_data);
		break;
	case BT_AD_SERVICE_DATA16:
	case BT_AD_SERVICE_DATA32:
	case BT_AD_SERVICE_DATA128:
		matched = queue_find(ad->service_data, match_service,
				user_data);
		break;
	default:
		matched = queue_find(ad->data, match_ad_data, user_data);
		break;
	}

	if (matched)
		info->matched_pattern = info->current_pattern;
}

struct bt_ad_pattern *bt_ad_pattern_match(struct bt_ad *ad,
							struct queue *patterns)
{
	struct pattern_match_info info;

	if (!ad || queue_isempty(patterns))
		return NULL;

	info.ad = ad;
	info.matched_pattern = NULL;
	info.current_pattern = NULL;

	queue_foreach(patterns, pattern_match, &info);

	return info.matched_pattern;
}
