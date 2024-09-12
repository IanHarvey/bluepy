/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *
 */

/* Response Status Code */
#define BT_ASCS_RSP_SUCCESS		0x00
#define BT_ASCS_RSP_NOT_SUPPORTED	0x01
#define BT_ASCS_RSP_TRUNCATED		0x02
#define BT_ASCS_RSP_INVALID_ASE		0x03
#define BT_ASCS_RSP_INVALID_ASE_STATE	0x04
#define BT_ASCS_RSP_INVALID_DIR		0x05
#define BT_ASCS_RSP_CAP_UNSUPPORTED	0x06
#define BT_ASCS_RSP_CONF_UNSUPPORTED	0x07
#define BT_ASCS_RSP_CONF_REJECTED	0x08
#define BT_ASCS_RSP_CONF_INVALID	0x09
#define BT_ASCS_RSP_METADATA_UNSUPPORTED 0x0a
#define BT_ASCS_RSP_METADATA_REJECTED	0x0b
#define BT_ASCS_RSP_METADATA_INVALID	0x0c
#define BT_ASCS_RSP_NO_MEM		0x0d
#define BT_ASCS_RSP_UNSPECIFIED		0x0e

/* Response Reasons */
#define BT_ASCS_REASON_NONE		0x00
#define BT_ASCS_REASON_CODEC		0x01
#define BT_ASCS_REASON_CODEC_DATA	0x02
#define BT_ASCS_REASON_INTERVAL		0x03
#define BT_ASCS_REASON_FRAMING		0x04
#define BT_ASCS_REASON_PHY		0x05
#define BT_ASCS_REASON_SDU		0x06
#define BT_ASCS_REASON_RTN		0x07
#define BT_ASCS_REASON_LATENCY		0x08
#define BT_ASCS_REASON_PD		0x09
#define BT_ASCS_REASON_CIS		0x0a

/* Transport QoS Packing */
#define BT_ASCS_QOS_PACKING_SEQ		0x00
#define BT_ASCS_QOS_PACKING_INT		0x01

/* Transport QoS Framing */
#define BT_ASCS_QOS_FRAMING_UNFRAMED	0x00
#define BT_ASCS_QOS_FRAMING_FRAMED	0x01

/* ASE characteristic states */
#define BT_ASCS_ASE_STATE_IDLE		0x00
#define BT_ASCS_ASE_STATE_CONFIG	0x01
#define BT_ASCS_ASE_STATE_QOS		0x02
#define BT_ASCS_ASE_STATE_ENABLING	0x03
#define BT_ASCS_ASE_STATE_STREAMING	0x04
#define BT_ASCS_ASE_STATE_DISABLING	0x05
#define BT_ASCS_ASE_STATE_RELEASING	0x06

struct bt_ascs_ase_rsp {
	uint8_t  ase;
	uint8_t  code;
	uint8_t  reason;
} __packed;

struct bt_ascs_cp_rsp {
	uint8_t  op;
	uint8_t  num_ase;
	struct bt_ascs_ase_rsp rsp[0];
} __packed;

struct bt_ascs_ase_status {
	uint8_t  id;
	uint8_t  state;
	uint8_t  params[0];
} __packed;

/* ASE_State = 0x01 (Codec Configured), defined in Table 4.7. */
struct bt_ascs_ase_status_config {
	uint8_t  framing;
	uint8_t  phy;
	uint8_t  rtn;
	uint16_t latency;
	uint8_t  pd_min[3];
	uint8_t  pd_max[3];
	uint8_t  ppd_min[3];
	uint8_t  ppd_max[3];
	struct bt_bap_codec codec;
	uint8_t  cc_len;
	/* LTV-formatted Codec-Specific Configuration */
	struct bt_ltv cc[0];
} __packed;

/* ASE_State = 0x02 (QoS Configured), defined in Table 4.8. */
struct bt_ascs_ase_status_qos {
	uint8_t  cig_id;
	uint8_t  cis_id;
	uint8_t  interval[3];
	uint8_t  framing;
	uint8_t  phy;
	uint16_t sdu;
	uint8_t  rtn;
	uint16_t latency;
	uint8_t  pd[3];
} __packed;

/* ASE_Status = 0x03 (Enabling), 0x04 (Streaming), or 0x05 (Disabling)
 * defined in Table 4.9.
 */
struct bt_ascs_ase_status_metadata {
	uint8_t  cig_id;
	uint8_t  cis_id;
	uint8_t  len;
	uint8_t  data[0];
} __packed;

struct bt_ascs_ase_hdr {
	uint8_t  op;
	uint8_t  num;
} __packed;

#define BT_ASCS_CONFIG			0x01

#define BT_ASCS_CONFIG_LATENCY_LOW	0x01
#define BT_ASCS_CONFIG_LATENCY_MEDIUM	0x02
#define BT_ASCS_CONFIG_LATENCY_HIGH	0x03

#define BT_ASCS_CONFIG_PHY_LE_1M	0x01
#define BT_ASCS_CONFIG_PHY_LE_2M	0x02
#define BT_ASCS_CONFIG_PHY_LE_CODED	0x03

struct bt_ascs_codec_config {
	uint8_t len;
	uint8_t type;
	uint8_t data[0];
} __packed;

struct bt_ascs_config {
	uint8_t  ase;			/* ASE ID */
	uint8_t  latency;		/* Target Latency */
	uint8_t  phy;			/* Target PHY */
	struct bt_bap_codec codec;	/* Codec ID */
	uint8_t  cc_len;		/* Codec Specific Config Length */
	/* LTV-formatted Codec-Specific Configuration */
	struct bt_ascs_codec_config cc[0];
} __packed;

#define BT_ASCS_QOS			0x02

struct bt_ascs_qos {
	uint8_t  ase;			/* ASE ID */
	uint8_t  cig;			/* CIG ID*/
	uint8_t  cis;			/* CIG ID*/
	uint8_t  interval[3];		/* Frame interval */
	uint8_t  framing;		/* Frame framing */
	uint8_t  phy;			/* PHY */
	uint16_t sdu;			/* Maximum SDU Size */
	uint8_t  rtn;			/* Retransmission Effort */
	uint16_t latency;		/* Transport Latency */
	uint8_t  pd[3];			/* Presentation Delay */
} __packed;

#define BT_ASCS_ENABLE			0x03

struct bt_ascs_metadata {
	uint8_t  ase;			/* ASE ID */
	uint8_t  len;			/* Metadata length */
	uint8_t  data[0];		/* LTV-formatted Metadata */
} __packed;

struct bt_ascs_enable {
	struct bt_ascs_metadata meta;	/* Metadata */
} __packed;

#define BT_ASCS_START			0x04

struct bt_ascs_start {
	uint8_t  ase;			/* ASE ID */
} __packed;

#define BT_ASCS_DISABLE			0x05

struct bt_ascs_disable {
	uint8_t  ase;			/* ASE ID */
} __packed;

#define BT_ASCS_STOP			0x06

struct bt_ascs_stop {
	uint8_t  ase;			/* ASE ID */
} __packed;

#define BT_ASCS_METADATA		0x07

#define BT_ASCS_RELEASE			0x08

struct bt_ascs_release {
	uint8_t  ase;			/* ASE ID */
} __packed;
