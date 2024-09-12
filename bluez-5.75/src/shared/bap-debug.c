// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  Intel Corporation.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include "src/shared/util.h"
#include "src/shared/bap-debug.h"

static const struct util_bit_debugger pac_freq_table[] = {
	UTIL_BIT_DEBUG(0, "8 Khz (0x0001)"),
	UTIL_BIT_DEBUG(1, "11.25 Khz (0x0002)"),
	UTIL_BIT_DEBUG(2, "16 Khz (0x0004)"),
	UTIL_BIT_DEBUG(3, "22.05 Khz (0x0008)"),
	UTIL_BIT_DEBUG(4, "24 Khz (0x0010)"),
	UTIL_BIT_DEBUG(5, "32 Khz (0x0020)"),
	UTIL_BIT_DEBUG(6, "44.1 Khz (0x0040)"),
	UTIL_BIT_DEBUG(7, "48 Khz (0x0080)"),
	UTIL_BIT_DEBUG(8, "88.2 Khz (0x0100)"),
	UTIL_BIT_DEBUG(9, "96 Khz (0x0200)"),
	UTIL_BIT_DEBUG(10, "176.4 Khz (0x0400)"),
	UTIL_BIT_DEBUG(11, "192 Khz (0x0800)"),
	UTIL_BIT_DEBUG(12, "384 Khz (0x1000)"),
	UTIL_BIT_DEBUG(13, "RFU (0x2000)"),
	UTIL_BIT_DEBUG(14, "RFU (0x4000)"),
	UTIL_BIT_DEBUG(15, "RFU (0x8000)"),
	{ }
};

static void pac_debug_freq(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint16_t value;
	uint16_t mask;

	if (!util_iov_pull_le16(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Sampling Frequencies: 0x%4.4x", value);

	mask = util_debug_bit("Sampling Frequency: ", value, pac_freq_table,
				func, user_data);
	if (mask)
		util_debug(func, user_data, "Unknown fields (0x%4.4x)",
						mask);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
					user_data);
}

static const struct util_bit_debugger pac_duration_table[] = {
	UTIL_BIT_DEBUG(0, "7.5 ms (0x01)"),
	UTIL_BIT_DEBUG(1, "10 ms (0x02)"),
	UTIL_BIT_DEBUG(2, "RFU (0x04)"),
	UTIL_BIT_DEBUG(3, "RFU (0x08)"),
	UTIL_BIT_DEBUG(4, "7.5 ms preferred (0x10)"),
	UTIL_BIT_DEBUG(5, "10 ms preferred (0x20)"),
	UTIL_BIT_DEBUG(6, "RFU (0x40)"),
	UTIL_BIT_DEBUG(7, "RFU (0x80)"),
	{ }
};

static void pac_debug_duration(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;
	uint8_t mask;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Frame Duration: 0x%2.2x", value);

	mask = util_debug_bit("Frame Duration: ", value, pac_duration_table,
				func, user_data);
	if (mask)
		util_debug(func, user_data, "Unknown fields (0x%2.2x)",
					mask);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
					user_data);
}

static const struct util_bit_debugger pac_channel_table[] = {
	UTIL_BIT_DEBUG(0, "1 channel (0x01)"),
	UTIL_BIT_DEBUG(1, "2 channel (0x02)"),
	UTIL_BIT_DEBUG(2, "3 channel (0x04)"),
	UTIL_BIT_DEBUG(3, "4 channel (0x08)"),
	UTIL_BIT_DEBUG(4, "5 channel (0x10)"),
	UTIL_BIT_DEBUG(5, "6 channel (0x20)"),
	UTIL_BIT_DEBUG(6, "7 channel (0x40)"),
	UTIL_BIT_DEBUG(7, "8 channel (0x80)"),
	{ }
};

static void pac_debug_channels(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;
	uint8_t mask;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Audio Channel Count: 0x%2.2x", value);

	mask = util_debug_bit("Audio Channel Count: ", value,
				pac_channel_table, func, user_data);
	if (mask)
		util_debug(func, user_data, "Unknown fields (0x%2.2x)",
					mask);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
					user_data);
}

static void pac_debug_frame_length(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint16_t min, max;

	if (!util_iov_pull_le16(&frame, &min)) {
		util_debug(func, user_data, "min: invalid size");
		goto done;
	}

	if (!util_iov_pull_le16(&frame, &max)) {
		util_debug(func, user_data, "max: invalid size");
		goto done;
	}

	util_debug(func, user_data,
			"Frame Length: %u (0x%4.4x) - %u (0x%4.4x)",
			min, min, max, max);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
					user_data);
}

static void pac_debug_sdu(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Max SDU: %u (0x%2.2x)", value, value);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
					user_data);
}

static const struct util_ltv_debugger pac_cap_table[] = {
	UTIL_LTV_DEBUG(0x01, pac_debug_freq),
	UTIL_LTV_DEBUG(0x02, pac_debug_duration),
	UTIL_LTV_DEBUG(0x03, pac_debug_channels),
	UTIL_LTV_DEBUG(0x04, pac_debug_frame_length),
	UTIL_LTV_DEBUG(0x05, pac_debug_sdu)
};

bool bt_bap_debug_caps(void *data, size_t len, util_debug_func_t func,
						void *user_data)
{
	return util_debug_ltv(data, len, pac_cap_table,
				ARRAY_SIZE(pac_cap_table),
				func, user_data);
}

static void ase_debug_freq(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	switch (value) {
	case 0x01:
		util_debug(func, user_data, "Sampling Frequency: 8 Khz (0x01)");
		break;
	case 0x02:
		util_debug(func, user_data,
				"Sampling Frequency: 11.25 Khz (0x02)");
		break;
	case 0x03:
		util_debug(func, user_data,
				"Sampling Frequency: 16 Khz (0x03)");
		break;
	case 0x04:
		util_debug(func, user_data,
				"Sampling Frequency: 22.05 Khz (0x04)");
		break;
	case 0x05:
		util_debug(func, user_data,
				"Sampling Frequency: 24 Khz (0x05)");
		break;
	case 0x06:
		util_debug(func, user_data,
				"Sampling Frequency: 32 Khz (0x06)");
		break;
	case 0x07:
		util_debug(func, user_data,
				"Sampling Frequency: 44.1 Khz (0x07)");
		break;
	case 0x08:
		util_debug(func, user_data,
				"Sampling Frequency: 48 Khz (0x08)");
		break;
	case 0x09:
		util_debug(func, user_data,
				"Sampling Frequency: 88.2 Khz (0x09)");
		break;
	case 0x0a:
		util_debug(func, user_data,
				"Sampling Frequency: 96 Khz (0x0a)");
		break;
	case 0x0b:
		util_debug(func, user_data,
				"Sampling Frequency: 176.4 Khz (0x0b)");
		break;
	case 0x0c:
		util_debug(func, user_data,
				"Sampling Frequency: 192 Khz (0x0c)");
		break;
	case 0x0d:
		util_debug(func, user_data,
				"Sampling Frequency: 384 Khz (0x0d)");
		break;
	default:
		util_debug(func, user_data,
				"Sampling Frequency: RFU (0x%2.2x)", value);
		break;
	}

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static void ase_debug_duration(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "\tvalue: invalid size\n");
		goto done;
	}

	switch (value) {
	case 0x00:
		util_debug(func, user_data, "Frame Duration: 7.5 ms (0x00)");
		break;
	case 0x01:
		util_debug(func, user_data, "Frame Duration: 10 ms (0x01)");
		break;
	default:
		util_debug(func, user_data, "Frame Duration: RFU (0x%2.2x)",
				value);
		break;
	}

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static const struct util_bit_debugger channel_location_table[] = {
	UTIL_BIT_DEBUG(0, "Front Left (0x00000001)"),
	UTIL_BIT_DEBUG(1, "Front Right (0x00000002)"),
	UTIL_BIT_DEBUG(2, "Front Center (0x00000004)"),
	UTIL_BIT_DEBUG(3, "Low Frequency Effects 1 (0x00000008)"),
	UTIL_BIT_DEBUG(4, "Back Left (0x00000010)"),
	UTIL_BIT_DEBUG(5, "Back Right (0x00000020)"),
	UTIL_BIT_DEBUG(6, "Front Left of Center (0x00000040)"),
	UTIL_BIT_DEBUG(7, "Front Right of Center (0x00000080)"),
	UTIL_BIT_DEBUG(8, "Back Center (0x00000100)"),
	UTIL_BIT_DEBUG(9, "Low Frequency Effects 2 (0x00000200)"),
	UTIL_BIT_DEBUG(10, "Side Left (0x00000400)"),
	UTIL_BIT_DEBUG(11, "Side Right (0x00000800)"),
	UTIL_BIT_DEBUG(12, "Top Front Left (0x00001000)"),
	UTIL_BIT_DEBUG(13, "Top Front Right (0x00002000)"),
	UTIL_BIT_DEBUG(14, "Top Front Center (0x00004000)"),
	UTIL_BIT_DEBUG(15, "Top Center (0x00008000)"),
	UTIL_BIT_DEBUG(16, "Top Back Left (0x00010000)"),
	UTIL_BIT_DEBUG(17, "Top Back Right (0x00020000)"),
	UTIL_BIT_DEBUG(18, "Top Side Left (0x00040000)"),
	UTIL_BIT_DEBUG(19, "Top Side Right (0x00080000)"),
	UTIL_BIT_DEBUG(20, "Top Back Center (0x00100000)"),
	UTIL_BIT_DEBUG(21, "Bottom Front Center (0x00200000)"),
	UTIL_BIT_DEBUG(22, "Bottom Front Left (0x00400000)"),
	UTIL_BIT_DEBUG(23, "Bottom Front Right (0x00800000)"),
	UTIL_BIT_DEBUG(24, "Front Left Wide (0x01000000)"),
	UTIL_BIT_DEBUG(25, "Front Right Wide (0x02000000)"),
	UTIL_BIT_DEBUG(26, "Left Surround (0x04000000)"),
	UTIL_BIT_DEBUG(27, "Right Surround (0x08000000)"),
	UTIL_BIT_DEBUG(28, "RFU (0x10000000)"),
	UTIL_BIT_DEBUG(29, "RFU (0x20000000)"),
	UTIL_BIT_DEBUG(30, "RFU (0x40000000)"),
	UTIL_BIT_DEBUG(31, "RFU (0x80000000)"),
	{ }
};

static void debug_location(const struct iovec *frame, util_debug_func_t func,
				void *user_data)
{
	uint32_t value;
	uint32_t mask;

	if (!util_iov_pull_le32((void *)frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Location: 0x%8.8x", value);

	mask = util_debug_bit("Location: ", value, channel_location_table,
				func, user_data);
	if (mask)
		util_debug(func, user_data, "Unknown fields (0x%8.8x)", mask);

done:
	if (frame->iov_len)
		util_hexdump(' ', frame->iov_base, frame->iov_len, func,
				user_data);
}

static void ase_debug_location(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };

	debug_location(&frame, func, user_data);
}

static void ase_debug_frame_length(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint16_t value;

	if (!util_iov_pull_le16(&frame, &value)) {
		util_debug(func, user_data, "\tvalue: invalid size\n");
		goto done;
	}

	util_debug(func, user_data, "Frame Length: %u (0x%4.4x)",
				value, value);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static void ase_debug_blocks(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint8_t value;

	if (!util_iov_pull_u8(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Frame Blocks per SDU: %u (0x%2.2x)",
				value, value);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static const struct util_ltv_debugger ase_cc_table[] = {
	UTIL_LTV_DEBUG(0x01, ase_debug_freq),
	UTIL_LTV_DEBUG(0x02, ase_debug_duration),
	UTIL_LTV_DEBUG(0x03, ase_debug_location),
	UTIL_LTV_DEBUG(0x04, ase_debug_frame_length),
	UTIL_LTV_DEBUG(0x05, ase_debug_blocks)
};

bool bt_bap_debug_config(void *data, size_t len, util_debug_func_t func,
						void *user_data)
{
	return util_debug_ltv(data, len, ase_cc_table,
				ARRAY_SIZE(ase_cc_table),
				func, user_data);
}

static const struct util_bit_debugger pac_context_table[] = {
	UTIL_BIT_DEBUG(0, "\tUnspecified (0x0001)"),
	UTIL_BIT_DEBUG(1, "\tConversational (0x0002)"),
	UTIL_BIT_DEBUG(2, "\tMedia (0x0004)"),
	UTIL_BIT_DEBUG(3, "\tGame (0x0008)"),
	UTIL_BIT_DEBUG(4, "\tInstructional (0x0010)"),
	UTIL_BIT_DEBUG(5, "\tVoice Assistants (0x0020)"),
	UTIL_BIT_DEBUG(6, "\tLive (0x0040)"),
	UTIL_BIT_DEBUG(7, "\tSound Effects (0x0080)"),
	UTIL_BIT_DEBUG(8, "\tNotifications (0x0100)"),
	UTIL_BIT_DEBUG(9, "\tRingtone (0x0200)"),
	UTIL_BIT_DEBUG(10, "\tAlerts (0x0400)"),
	UTIL_BIT_DEBUG(11, "\tEmergency alarm (0x0800)"),
	UTIL_BIT_DEBUG(12, "\tRFU (0x1000)"),
	UTIL_BIT_DEBUG(13, "\tRFU (0x2000)"),
	UTIL_BIT_DEBUG(14, "\tRFU (0x4000)"),
	UTIL_BIT_DEBUG(15, "\tRFU (0x8000)"),
	{ }
};

static void debug_context(const struct iovec *frame, const char *label,
				util_debug_func_t func, void *user_data)
{
	uint16_t value;
	uint16_t mask;

	if (!util_iov_pull_le16((void *)frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "%s: 0x%4.4x", label, value);

	mask = util_debug_bit(label, value, pac_context_table, func, user_data);
	if (mask)
		util_debug(func, user_data, "Unknown fields (0x%4.4x)", mask);

done:
	if (frame->iov_len)
		util_hexdump(' ', frame->iov_base, frame->iov_len, func,
				user_data);
}

static void ase_debug_preferred_context(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };

	debug_context(&frame, "Preferred Context", func, user_data);
}

static void ase_debug_context(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };

	debug_context(&frame, "Context", func, user_data);
}

static void ase_debug_program_info(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	const char *str;

	str = util_iov_pull_mem(&frame, len);
	if (!str) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Program Info: %*s", len, str);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static void ase_debug_language(const uint8_t *data, uint8_t len,
				util_debug_func_t func, void *user_data)
{
	struct iovec frame = { (void *)data, len };
	uint32_t value;

	if (!util_iov_pull_le24(&frame, &value)) {
		util_debug(func, user_data, "value: invalid size");
		goto done;
	}

	util_debug(func, user_data, "Language: 0x%6.6x\n", value);

done:
	if (frame.iov_len)
		util_hexdump(' ', frame.iov_base, frame.iov_len, func,
				user_data);
}

static const struct util_ltv_debugger ase_metadata_table[] = {
	UTIL_LTV_DEBUG(0x01, ase_debug_preferred_context),
	UTIL_LTV_DEBUG(0x02, ase_debug_context),
	UTIL_LTV_DEBUG(0x03, ase_debug_program_info),
	UTIL_LTV_DEBUG(0x04, ase_debug_language)
};

bool bt_bap_debug_metadata(void *data, size_t len, util_debug_func_t func,
						void *user_data)
{
	return util_debug_ltv(data, len, ase_metadata_table,
				ARRAY_SIZE(ase_metadata_table),
				func, user_data);
}
