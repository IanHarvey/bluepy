/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *  Copyright 2023-2024 NXP
 *
 */

#ifndef SRC_SHARED_BAP_DEFS_H_
#define SRC_SHARED_BAP_DEFS_H_

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#define BT_BAP_SINK			0x01
#define	BT_BAP_SOURCE			0x02
#define	BT_BAP_BCAST_SOURCE		0x03
#define	BT_BAP_BCAST_SINK		0x04

#define BT_BAP_STREAM_TYPE_UCAST	0x01
#define	BT_BAP_STREAM_TYPE_BCAST	0x02

#define BT_BAP_STREAM_STATE_IDLE	0x00
#define BT_BAP_STREAM_STATE_CONFIG	0x01
#define BT_BAP_STREAM_STATE_QOS		0x02
#define BT_BAP_STREAM_STATE_ENABLING	0x03
#define BT_BAP_STREAM_STATE_STREAMING	0x04
#define BT_BAP_STREAM_STATE_DISABLING	0x05
#define BT_BAP_STREAM_STATE_RELEASING	0x06

#define BT_BAP_CONFIG_LATENCY_LOW	0x01
#define BT_BAP_CONFIG_LATENCY_BALANCED	0x02
#define BT_BAP_CONFIG_LATENCY_HIGH	0x03

#define BT_BAP_CONFIG_PHY_1M		0x01
#define BT_BAP_CONFIG_PHY_2M		0x02
#define BT_BAP_CONFIG_PHY_CODEC		0x03

struct bt_bap_codec {
	uint8_t  id;
	uint16_t cid;
	uint16_t vid;
} __packed;

struct bt_ltv {
	uint8_t  len;
	uint8_t  type;
	uint8_t  value[];
} __packed;

struct bt_bap_io_qos {
	uint32_t interval;	/* Frame interval */
	uint16_t latency;	/* Transport Latency */
	uint16_t sdu;		/* Maximum SDU Size */
	uint8_t  phy;		/* PHY */
	uint8_t  rtn;		/* Retransmission Effort */
};

struct bt_bap_ucast_qos {
	uint8_t  cig_id;
	uint8_t  cis_id;
	uint8_t  framing;		/* Frame framing */
	uint32_t delay;			/* Presentation Delay */
	uint8_t  target_latency;	/* Target Latency */
	struct bt_bap_io_qos io_qos;
};

struct bt_bap_bcast_qos {
	uint8_t  big;
	uint8_t  bis;
	uint8_t  sync_factor;
	uint8_t  packing;
	uint8_t  framing;
	uint8_t  encryption;
	struct iovec *bcode;
	uint8_t  options;
	uint16_t skip;
	uint16_t sync_timeout;
	uint8_t  sync_cte_type;
	uint8_t  mse;
	uint16_t timeout;
	uint8_t  pa_sync;
	struct bt_bap_io_qos io_qos;
	uint32_t delay;			/* Presentation Delay */
};

struct bt_bap_qos {
	union {
		struct bt_bap_ucast_qos ucast;
		struct bt_bap_bcast_qos bcast;
	};
};

#endif /* SRC_SHARED_BAP_DEFS_H_ */
