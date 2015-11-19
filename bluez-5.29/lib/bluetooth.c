/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "bluetooth.h"
#include "hci.h"

void baswap(bdaddr_t *dst, const bdaddr_t *src)
{
	register unsigned char *d = (unsigned char *) dst;
	register const unsigned char *s = (const unsigned char *) src;
	register int i;

	for (i = 0; i < 6; i++)
		d[i] = s[5-i];
}

char *batostr(const bdaddr_t *ba)
{
	char *str = bt_malloc(18);
	if (!str)
		return NULL;

	sprintf(str, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
		ba->b[0], ba->b[1], ba->b[2],
		ba->b[3], ba->b[4], ba->b[5]);

	return str;
}

bdaddr_t *strtoba(const char *str)
{
	bdaddr_t b;
	bdaddr_t *ba = bt_malloc(sizeof(*ba));

	if (ba) {
		str2ba(str, &b);
		baswap(ba, &b);
	}

	return ba;
}

int ba2str(const bdaddr_t *ba, char *str)
{
	return sprintf(str, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
		ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

int str2ba(const char *str, bdaddr_t *ba)
{
	int i;

	if (bachk(str) < 0) {
		memset(ba, 0, sizeof(*ba));
		return -1;
	}

	for (i = 5; i >= 0; i--, str += 3)
		ba->b[i] = strtol(str, NULL, 16);

	return 0;
}

int ba2oui(const bdaddr_t *ba, char *str)
{
	return sprintf(str, "%2.2X-%2.2X-%2.2X", ba->b[5], ba->b[4], ba->b[3]);
}

int bachk(const char *str)
{
	if (!str)
		return -1;

	if (strlen(str) != 17)
		return -1;

	while (*str) {
		if (!isxdigit(*str++))
			return -1;

		if (!isxdigit(*str++))
			return -1;

		if (*str == 0)
			break;

		if (*str++ != ':')
			return -1;
	}

	return 0;
}

int baprintf(const char *format, ...)
{
	va_list ap;
	int len;

	va_start(ap, format);
	len = vprintf(format, ap);
	va_end(ap);

	return len;
}

int bafprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int len;

	va_start(ap, format);
	len = vfprintf(stream, format, ap);
	va_end(ap);

	return len;
}

int basprintf(char *str, const char *format, ...)
{
	va_list ap;
	int len;

	va_start(ap, format);
	len = vsnprintf(str, (~0U) >> 1, format, ap);
	va_end(ap);

	return len;
}

int basnprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int len;

	va_start(ap, format);
	len = vsnprintf(str, size, format, ap);
	va_end(ap);

	return len;
}

void *bt_malloc(size_t size)
{
	return malloc(size);
}

void bt_free(void *ptr)
{
	free(ptr);
}

/* Bluetooth error codes to Unix errno mapping */
int bt_error(uint16_t code)
{
	switch (code) {
	case 0:
		return 0;
	case HCI_UNKNOWN_COMMAND:
		return EBADRQC;
	case HCI_NO_CONNECTION:
		return ENOTCONN;
	case HCI_HARDWARE_FAILURE:
		return EIO;
	case HCI_PAGE_TIMEOUT:
		return EHOSTDOWN;
	case HCI_AUTHENTICATION_FAILURE:
		return EACCES;
	case HCI_PIN_OR_KEY_MISSING:
		return EINVAL;
	case HCI_MEMORY_FULL:
		return ENOMEM;
	case HCI_CONNECTION_TIMEOUT:
		return ETIMEDOUT;
	case HCI_MAX_NUMBER_OF_CONNECTIONS:
	case HCI_MAX_NUMBER_OF_SCO_CONNECTIONS:
		return EMLINK;
	case HCI_ACL_CONNECTION_EXISTS:
		return EALREADY;
	case HCI_COMMAND_DISALLOWED:
	case HCI_TRANSACTION_COLLISION:
	case HCI_ROLE_SWITCH_PENDING:
		return EBUSY;
	case HCI_REJECTED_LIMITED_RESOURCES:
	case HCI_REJECTED_PERSONAL:
	case HCI_QOS_REJECTED:
		return ECONNREFUSED;
	case HCI_HOST_TIMEOUT:
		return ETIMEDOUT;
	case HCI_UNSUPPORTED_FEATURE:
	case HCI_QOS_NOT_SUPPORTED:
	case HCI_PAIRING_NOT_SUPPORTED:
	case HCI_CLASSIFICATION_NOT_SUPPORTED:
	case HCI_UNSUPPORTED_LMP_PARAMETER_VALUE:
	case HCI_PARAMETER_OUT_OF_RANGE:
	case HCI_QOS_UNACCEPTABLE_PARAMETER:
		return EOPNOTSUPP;
	case HCI_INVALID_PARAMETERS:
	case HCI_SLOT_VIOLATION:
		return EINVAL;
	case HCI_OE_USER_ENDED_CONNECTION:
	case HCI_OE_LOW_RESOURCES:
	case HCI_OE_POWER_OFF:
		return ECONNRESET;
	case HCI_CONNECTION_TERMINATED:
		return ECONNABORTED;
	case HCI_REPEATED_ATTEMPTS:
		return ELOOP;
	case HCI_REJECTED_SECURITY:
	case HCI_PAIRING_NOT_ALLOWED:
	case HCI_INSUFFICIENT_SECURITY:
		return EACCES;
	case HCI_UNSUPPORTED_REMOTE_FEATURE:
		return EPROTONOSUPPORT;
	case HCI_SCO_OFFSET_REJECTED:
		return ECONNREFUSED;
	case HCI_UNKNOWN_LMP_PDU:
	case HCI_INVALID_LMP_PARAMETERS:
	case HCI_LMP_ERROR_TRANSACTION_COLLISION:
	case HCI_LMP_PDU_NOT_ALLOWED:
	case HCI_ENCRYPTION_MODE_NOT_ACCEPTED:
		return EPROTO;
	default:
		return ENOSYS;
	}
}

const char *bt_compidtostr(int compid)
{
	switch (compid) {
	case 0:
		return "Ericsson Technology Licensing";
	case 1:
		return "Nokia Mobile Phones";
	case 2:
		return "Intel Corp.";
	case 3:
		return "IBM Corp.";
	case 4:
		return "Toshiba Corp.";
	case 5:
		return "3Com";
	case 6:
		return "Microsoft";
	case 7:
		return "Lucent";
	case 8:
		return "Motorola";
	case 9:
		return "Infineon Technologies AG";
	case 10:
		return "Cambridge Silicon Radio";
	case 11:
		return "Silicon Wave";
	case 12:
		return "Digianswer A/S";
	case 13:
		return "Texas Instruments Inc.";
	case 14:
		return "Ceva, Inc. (formerly Parthus Technologies, Inc.)";
	case 15:
		return "Broadcom Corporation";
	case 16:
		return "Mitel Semiconductor";
	case 17:
		return "Widcomm, Inc";
	case 18:
		return "Zeevo, Inc.";
	case 19:
		return "Atmel Corporation";
	case 20:
		return "Mitsubishi Electric Corporation";
	case 21:
		return "RTX Telecom A/S";
	case 22:
		return "KC Technology Inc.";
	case 23:
		return "NewLogic";
	case 24:
		return "Transilica, Inc.";
	case 25:
		return "Rohde & Schwarz GmbH & Co. KG";
	case 26:
		return "TTPCom Limited";
	case 27:
		return "Signia Technologies, Inc.";
	case 28:
		return "Conexant Systems Inc.";
	case 29:
		return "Qualcomm";
	case 30:
		return "Inventel";
	case 31:
		return "AVM Berlin";
	case 32:
		return "BandSpeed, Inc.";
	case 33:
		return "Mansella Ltd";
	case 34:
		return "NEC Corporation";
	case 35:
		return "WavePlus Technology Co., Ltd.";
	case 36:
		return "Alcatel";
	case 37:
		return "NXP Semiconductors (formerly Philips Semiconductors)";
	case 38:
		return "C Technologies";
	case 39:
		return "Open Interface";
	case 40:
		return "R F Micro Devices";
	case 41:
		return "Hitachi Ltd";
	case 42:
		return "Symbol Technologies, Inc.";
	case 43:
		return "Tenovis";
	case 44:
		return "Macronix International Co. Ltd.";
	case 45:
		return "GCT Semiconductor";
	case 46:
		return "Norwood Systems";
	case 47:
		return "MewTel Technology Inc.";
	case 48:
		return "ST Microelectronics";
	case 49:
		return "Synopsis";
	case 50:
		return "Red-M (Communications) Ltd";
	case 51:
		return "Commil Ltd";
	case 52:
		return "Computer Access Technology Corporation (CATC)";
	case 53:
		return "Eclipse (HQ Espana) S.L.";
	case 54:
		return "Renesas Electronics Corporation";
	case 55:
		return "Mobilian Corporation";
	case 56:
		return "Terax";
	case 57:
		return "Integrated System Solution Corp.";
	case 58:
		return "Matsushita Electric Industrial Co., Ltd.";
	case 59:
		return "Gennum Corporation";
	case 60:
		return "BlackBerry Limited (formerly Research In Motion)";
	case 61:
		return "IPextreme, Inc.";
	case 62:
		return "Systems and Chips, Inc.";
	case 63:
		return "Bluetooth SIG, Inc.";
	case 64:
		return "Seiko Epson Corporation";
	case 65:
		return "Integrated Silicon Solution Taiwan, Inc.";
	case 66:
		return "CONWISE Technology Corporation Ltd";
	case 67:
		return "PARROT SA";
	case 68:
		return "Socket Mobile";
	case 69:
		return "Atheros Communications, Inc.";
	case 70:
		return "MediaTek, Inc.";
	case 71:
		return "Bluegiga";
	case 72:
		return "Marvell Technology Group Ltd.";
	case 73:
		return "3DSP Corporation";
	case 74:
		return "Accel Semiconductor Ltd.";
	case 75:
		return "Continental Automotive Systems";
	case 76:
		return "Apple, Inc.";
	case 77:
		return "Staccato Communications, Inc.";
	case 78:
		return "Avago Technologies";
	case 79:
		return "APT Licensing Ltd.";
	case 80:
		return "SiRF Technology";
	case 81:
		return "Tzero Technologies, Inc.";
	case 82:
		return "J&M Corporation";
	case 83:
		return "Free2move AB";
	case 84:
		return "3DiJoy Corporation";
	case 85:
		return "Plantronics, Inc.";
	case 86:
		return "Sony Ericsson Mobile Communications";
	case 87:
		return "Harman International Industries, Inc.";
	case 88:
		return "Vizio, Inc.";
	case 89:
		return "Nordic Semiconductor ASA";
	case 90:
		return "EM Microelectronic-Marin SA";
	case 91:
		return "Ralink Technology Corporation";
	case 92:
		return "Belkin International, Inc.";
	case 93:
		return "Realtek Semiconductor Corporation";
	case 94:
		return "Stonestreet One, LLC";
	case 95:
		return "Wicentric, Inc.";
	case 96:
		return "RivieraWaves S.A.S";
	case 97:
		return "RDA Microelectronics";
	case 98:
		return "Gibson Guitars";
	case 99:
		return "MiCommand Inc.";
	case 100:
		return "Band XI International, LLC";
	case 101:
		return "Hewlett-Packard Company";
	case 102:
		return "9Solutions Oy";
	case 103:
		return "GN Netcom A/S";
	case 104:
		return "General Motors";
	case 105:
		return "A&D Engineering, Inc.";
	case 106:
		return "MindTree Ltd.";
	case 107:
		return "Polar Electro OY";
	case 108:
		return "Beautiful Enterprise Co., Ltd.";
	case 109:
		return "BriarTek, Inc.";
	case 110:
		return "Summit Data Communications, Inc.";
	case 111:
		return "Sound ID";
	case 112:
		return "Monster, LLC";
	case 113:
		return "connectBlue AB";
	case 114:
		return "ShangHai Super Smart Electronics Co. Ltd.";
	case 115:
		return "Group Sense Ltd.";
	case 116:
		return "Zomm, LLC";
	case 117:
		return "Samsung Electronics Co. Ltd.";
	case 118:
		return "Creative Technology Ltd.";
	case 119:
		return "Laird Technologies";
	case 120:
		return "Nike, Inc.";
	case 121:
		return "lesswire AG";
	case 122:
		return "MStar Semiconductor, Inc.";
	case 123:
		return "Hanlynn Technologies";
	case 124:
		return "A & R Cambridge";
	case 125:
		return "Seers Technology Co. Ltd";
	case 126:
		return "Sports Tracking Technologies Ltd.";
	case 127:
		return "Autonet Mobile";
	case 128:
		return "DeLorme Publishing Company, Inc.";
	case 129:
		return "WuXi Vimicro";
	case 130:
		return "Sennheiser Communications A/S";
	case 131:
		return "TimeKeeping Systems, Inc.";
	case 132:
		return "Ludus Helsinki Ltd.";
	case 133:
		return "BlueRadios, Inc.";
	case 134:
		return "equinox AG";
	case 135:
		return "Garmin International, Inc.";
	case 136:
		return "Ecotest";
	case 137:
		return "GN ReSound A/S";
	case 138:
		return "Jawbone";
	case 139:
		return "Topcorn Positioning Systems, LLC";
	case 140:
		return "Gimbal Inc. (formerly Qualcomm Labs, Inc. and Qualcomm Retail Solutions, Inc.)";
	case 141:
		return "Zscan Software";
	case 142:
		return "Quintic Corp.";
	case 143:
		return "Stollman E+V GmbH";
	case 144:
		return "Funai Electric Co., Ltd.";
	case 145:
		return "Advanced PANMOBIL Systems GmbH & Co. KG";
	case 146:
		return "ThinkOptics, Inc.";
	case 147:
		return "Universal Electronics, Inc.";
	case 148:
		return "Airoha Technology Corp.";
	case 149:
		return "NEC Lighting, Ltd.";
	case 150:
		return "ODM Technology, Inc.";
	case 151:
		return "ConnecteDevice Ltd.";
	case 152:
		return "zer01.tv GmbH";
	case 153:
		return "i.Tech Dynamic Global Distribution Ltd.";
	case 154:
		return "Alpwise";
	case 155:
		return "Jiangsu Toppower Automotive Electronics Co., Ltd.";
	case 156:
		return "Colorfy, Inc.";
	case 157:
		return "Geoforce Inc.";
	case 158:
		return "Bose Corporation";
	case 159:
		return "Suunto Oy";
	case 160:
		return "Kensington Computer Products Group";
	case 161:
		return "SR-Medizinelektronik";
	case 162:
		return "Vertu Corporation Limited";
	case 163:
		return "Meta Watch Ltd.";
	case 164:
		return "LINAK A/S";
	case 165:
		return "OTL Dynamics LLC";
	case 166:
		return "Panda Ocean Inc.";
	case 167:
		return "Visteon Corporation";
	case 168:
		return "ARP Devices Limited";
	case 169:
		return "Magneti Marelli S.p.A";
	case 170:
		return "CAEN RFID srl";
	case 171:
		return "Ingenieur-Systemgruppe Zahn GmbH";
	case 172:
		return "Green Throttle Games";
	case 173:
		return "Peter Systemtechnik GmbH";
	case 174:
		return "Omegawave Oy";
	case 175:
		return "Cinetix";
	case 176:
		return "Passif Semiconductor Corp";
	case 177:
		return "Saris Cycling Group, Inc";
	case 178:
		return "Bekey A/S";
	case 179:
		return "Clarinox Technologies Pty. Ltd.";
	case 180:
		return "BDE Technology Co., Ltd.";
	case 181:
		return "Swirl Networks";
	case 182:
		return "Meso international";
	case 183:
		return "TreLab Ltd";
	case 184:
		return "Qualcomm Innovation Center, Inc. (QuIC)";
	case 185:
		return "Johnson Controls, Inc.";
	case 186:
		return "Starkey Laboratories Inc.";
	case 187:
		return "S-Power Electronics Limited";
	case 188:
		return "Ace Sensor Inc";
	case 189:
		return "Aplix Corporation";
	case 190:
		return "AAMP of America";
	case 191:
		return "Stalmart Technology Limited";
	case 192:
		return "AMICCOM Electronics Corporation";
	case 193:
		return "Shenzhen Excelsecu Data Technology Co.,Ltd";
	case 194:
		return "Geneq Inc.";
	case 195:
		return "adidas AG";
	case 196:
		return "LG Electronics";
	case 197:
		return "Onset Computer Corporation";
	case 198:
		return "Selfly BV";
	case 199:
		return "Quuppa Oy.";
	case 200:
		return "GeLo Inc";
	case 201:
		return "Evluma";
	case 202:
		return "MC10";
	case 203:
		return "Binauric SE";
	case 204:
		return "Beats Electronics";
	case 205:
		return "Microchip Technology Inc.";
	case 206:
		return "Elgato Systems GmbH";
	case 207:
		return "ARCHOS SA";
	case 208:
		return "Dexcom, Inc.";
	case 209:
		return "Polar Electro Europe B.V.";
	case 210:
		return "Dialog Semiconductor B.V.";
	case 211:
		return "Taixingbang Technology (HK) Co,. LTD.";
	case 212:
		return "Kawantech";
	case 213:
		return "Austco Communication Systems";
	case 214:
		return "Timex Group USA, Inc.";
	case 215:
		return "Qualcomm Technologies, Inc.";
	case 216:
		return "Qualcomm Connected Experiences, Inc.";
	case 217:
		return "Voyetra Turtle Beach";
	case 218:
		return "txtr GmbH";
	case 219:
		return "Biosentronics";
	case 220:
		return "Procter & Gamble";
	case 221:
		return "Hosiden Corporation";
	case 222:
		return "Muzik LLC";
	case 223:
		return "Misfit Wearables Corp";
	case 224:
		return "Google";
	case 225:
		return "Danlers Ltd";
	case 226:
		return "Semilink Inc";
	case 227:
		return "inMusic Brands, Inc";
	case 228:
		return "L.S. Research Inc.";
	case 229:
		return "Eden Software Consultants Ltd.";
	case 230:
		return "Freshtemp";
	case 231:
		return "KS Technologies";
	case 232:
		return "ACTS Technologies";
	case 233:
		return "Vtrack Systems";
	case 234:
		return "Nielsen-Kellerman Company";
	case 235:
		return "Server Technology, Inc.";
	case 236:
		return "BioResearch Associates";
	case 237:
		return "Jolly Logic, LLC";
	case 238:
		return "Above Average Outcomes, Inc.";
	case 239:
		return "Bitsplitters GmbH";
	case 240:
		return "PayPal, Inc.";
	case 241:
		return "Witron Technology Limited";
	case 242:
		return "Aether Things Inc. (formerly Morse Project Inc.)";
	case 243:
		return "Kent Displays Inc.";
	case 244:
		return "Nautilus Inc.";
	case 245:
		return "Smartifier Oy";
	case 246:
		return "Elcometer Limited";
	case 247:
		return "VSN Technologies Inc.";
	case 248:
		return "AceUni Corp., Ltd.";
	case 249:
		return "StickNFind";
	case 250:
		return "Crystal Code AB";
	case 251:
		return "KOUKAAM a.s.";
	case 252:
		return "Delphi Corporation";
	case 253:
		return "ValenceTech Limited";
	case 254:
		return "Reserved";
	case 255:
		return "Typo Products, LLC";
	case 256:
		return "TomTom International BV";
	case 257:
		return "Fugoo, Inc";
	case 258:
		return "Keiser Corporation";
	case 259:
		return "Bang & Olufsen A/S";
	case 260:
		return "PLUS Locations Systems Pty Ltd";
	case 261:
		return "Ubiquitous Computing Technology Corporation";
	case 262:
		return "Innovative Yachtter Solutions";
	case 263:
		return "William Demant Holding A/S";
	case 264:
		return "Chicony Electronics Co., Ltd.";
	case 265:
		return "Atus BV";
	case 266:
		return "Codegate Ltd.";
	case 267:
		return "ERi, Inc.";
	case 268:
		return "Transducers Direct, LLC";
	case 269:
		return "Fujitsu Ten Limited";
	case 270:
		return "Audi AG";
	case 271:
		return "HiSilicon Technologies Co., Ltd.";
	case 272:
		return "Nippon Seiki Co., Ltd.";
	case 273:
		return "Steelseries ApS";
	case 274:
		return "vyzybl Inc.";
	case 275:
		return "Openbrain Technologies, Co., Ltd.";
	case 276:
		return "Xensr";
	case 277:
		return "e.solutions";
	case 278:
		return "1OAK Technologies";
	case 279:
		return "Wimoto Technologies Inc";
	case 280:
		return "Radius Networks, Inc.";
	case 281:
		return "Wize Technology Co., Ltd.";
	case 282:
		return "Qualcomm Labs, Inc.";
	case 283:
		return "Aruba Networks";
	case 284:
		return "Baidu";
	case 285:
		return "Arendi AG";
	case 286:
		return "Skoda Auto a.s.";
	case 287:
		return "Volkswagon AG";
	case 288:
		return "Porsche AG";
	case 289:
		return "Sino Wealth Electronic Ltd.";
	case 290:
		return "AirTurn, Inc.";
	case 291:
		return "Kinsa, Inc.";
	case 292:
		return "HID Global";
	case 293:
		return "SEAT es";
	case 294:
		return "Promethean Ltd.";
	case 295:
		return "Salutica Allied Solutions";
	case 296:
		return "GPSI Group Pty Ltd";
	case 297:
		return "Nimble Devices Oy";
	case 298:
		return "Changzhou Yongse Infotech Co., Ltd";
	case 299:
		return "SportIQ";
	case 300:
		return "TEMEC Instruments B.V.";
	case 301:
		return "Sony Corporation";
	case 302:
		return "ASSA ABLOY";
	case 303:
		return "Clarion Co., Ltd.";
	case 304:
		return "Warehouse Innovations";
	case 305:
		return "Cypress Semiconductor Corporation";
	case 306:
		return "MADS Inc";
	case 307:
		return "Blue Maestro Limited";
	case 308:
		return "Resolution Products, Inc.";
	case 309:
		return "Airewear LLC";
	case 310:
		return "Seed Labs, Inc. (formerly ETC sp. z.o.o.)";
	case 311:
		return "Prestigio Plaza Ltd.";
	case 312:
		return "NTEO Inc.";
	case 313:
		return "Focus Systems Corporation";
	case 314:
		return "Tencent Holdings Limited";
	case 315:
		return "Allegion";
	case 316:
		return "Murata Manufacuring Co., Ltd.";
	case 317:
		return "WirelessWERX";
	case 318:
		return "Nod, Inc.";
	case 319:
		return "B&B Manufacturing Company";
	case 320:
		return "Alpine Electronics (China) Co., Ltd";
	case 321:
		return "FedEx Services";
	case 322:
		return "Grape Systems Inc.";
	case 323:
		return "Bkon Connect";
	case 324:
		return "Lintech GmbH";
	case 325:
		return "Novatel Wireless";
	case 326:
		return "Ciright";
	case 327:
		return "Mighty Cast, Inc.";
	case 328:
		return "Ambimat Electronics";
	case 329:
		return "Perytons Ltd.";
	case 330:
		return "Tivoli Audio, LLC";
	case 331:
		return "Master Lock";
	case 332:
		return "Mesh-Net Ltd";
	case 333:
		return "Huizhou Desay SV Automotive CO., LTD.";
	case 334:
		return "Tangerine, Inc.";
	case 335:
		return "B&W Group Ltd.";
	case 336:
		return "Pioneer Corporation";
	case 337:
		return "OnBeep";
	case 338:
		return "Vernier Software & Technology";
	case 339:
		return "ROL Ergo";
	case 340:
		return "Pebble Technology";
	case 341:
		return "NETATMO";
	case 342:
		return "Accumulate AB";
	case 343:
		return "Anhui Huami Information Technology Co., Ltd.";
	case 344:
		return "Inmite s.r.o.";
	case 345:
		return "ChefSteps, Inc.";
	case 346:
		return "micas AG";
	case 347:
		return "Biomedical Research Ltd.";
	case 348:
		return "Pitius Tec S.L.";
	case 349:
		return "Estimote, Inc.";
	case 350:
		return "Unikey Technologies, Inc.";
	case 351:
		return "Timer Cap Co.";
	case 352:
		return "AwoX";
	case 353:
		return "yikes";
	case 354:
		return "MADSGlobal NZ Ltd.";
	case 355:
		return "PCH International";
	case 356:
		return "Qingdao Yeelink Information Technology Co., Ltd.";
	case 357:
		return "Milwaukee Tool (formerly Milwaukee Electric Tools)";
	case 358:
		return "MISHIK Pte Ltd";
	case 359:
		return "Bayer HealthCare";
	case 360:
		return "Spicebox LLC";
	case 361:
		return "emberlight";
	case 362:
		return "Cooper-Atkins Corporation";
	case 363:
		return "Qblinks";
	case 364:
		return "MYSPHERA";
	case 365:
		return "LifeScan Inc";
	case 366:
		return "Volantic AB";
	case 367:
		return "Podo Labs, Inc";
	case 368:
		return "Roche Diabetes Care AG";
	case 369:
		return "Amazon Fulfillment Service";
	case 370:
		return "Connovate Technology Private Limited";
	case 371:
		return "Kocomojo, LLC";
	case 372:
		return "Everykey LLC";
	case 373:
		return "Dynamic Controls";
	case 374:
		return "SentriLock";
	case 375:
		return "I-SYST inc.";
	case 376:
		return "CASIO COMPUTER CO., LTD.";
	case 377:
		return "LAPIS Semiconductor Co., Ltd.";
	case 378:
		return "Telemonitor, Inc.";
	case 379:
		return "taskit GmbH";
	case 380:
		return "Daimler AG";
	case 381:
		return "BatAndCat";
	case 382:
		return "BluDotz Ltd";
	case 383:
		return "XTel ApS";
	case 384:
		return "Gigaset Communications GmbH";
	case 385:
		return "Gecko Health Innovations, Inc.";
	case 386:
		return "HOP Ubiquitous";
	case 387:
		return "To Be Assigned";
	case 388:
		return "Nectar";
	case 389:
		return "bel'apps LLC";
	case 390:
		return "CORE Lighting Ltd";
	case 391:
		return "Seraphim Sense Ltd";
	case 392:
		return "Unico RBC";
	case 393:
		return "Physical Enterprises Inc.";
	case 394:
		return "Able Trend Technology Limited";
	case 395:
		return "Konica Minolta, Inc.";
	case 396:
		return "Wilo SE";
	case 397:
		return "Extron Design Services";
	case 398:
		return "Fitbit, Inc.";
	case 399:
		return "Fireflies Systems";
	case 400:
		return "Intelletto Technologies Inc.";
	case 401:
		return "FDK CORPORATION";
	case 402:
		return "Cloudleaf, Inc";
	case 403:
		return "Maveric Automation LLC";
	case 404:
		return "Acoustic Stream Corporation";
	case 405:
		return "Zuli";
	case 406:
		return "Paxton Access Ltd";
	case 407:
		return "WiSilica Inc";
	case 408:
		return "Vengit Limited";
	case 409:
		return "SALTO SYSTEMS S.L.";
	case 410:
		return "T-Engine Forum";
	case 411:
		return "CUBETECH s.r.o.";
	case 412:
		return "Cokiya Incorporated";
	case 413:
		return "CVS Health";
	case 414:
		return "Ceruus";
	case 415:
		return "Strainstall Ltd";
	case 416:
		return "Channel Enterprises (HK) Ltd.";
	case 417:
		return "FIAMM";
	case 418:
		return "GIGALANE.CO.,LTD";
	case 419:
		return "EROAD";
	case 420:
		return "Mine Safety Appliances";
	case 421:
		return "Icon Health and Fitness";
	case 422:
		return "Asandoo GmbH";
	case 423:
		return "ENERGOUS CORPORATION";
	case 424:
		return "Taobao";
	case 425:
		return "Canon Inc.";
	case 426:
		return "Geophysical Technology Inc.";
	case 427:
		return "Facebook, Inc.";
	case 428:
		return "Nipro Diagnostics, Inc.";
	case 429:
		return "FlightSafety International";
	case 430:
		return "Earlens Corporation";
	case 431:
		return "Sunrise Micro Devices, Inc.";
	case 432:
		return "Star Micronics Co., Ltd.";
	case 433:
		return "Netizens Sp. z o.o.";
	case 434:
		return "Nymi Inc.";
	case 435:
		return "Nytec, Inc.";
	case 436:
		return "Trineo Sp. z o.o.";
	case 437:
		return "Nest Labs Inc.";
	case 438:
		return "LM Technologies Ltd";
	case 439:
		return "General Electric Company";
	case 440:
		return "i+D3 S.L.";
	case 441:
		return "HANA Micron";
	case 442:
		return "Stages Cycling LLC";
	case 443:
		return "Cochlear Bone Anchored Solutions AB";
	case 444:
		return "SenionLab AB";
	case 445:
		return "Syszone Co., Ltd";
	case 446:
		return "Pulsate Mobile Ltd.";
	case 447:
		return "Hong Kong HunterSun Electronic Limited";
	case 448:
		return "pironex GmbH";
	case 449:
		return "BRADATECH Corp.";
	case 450:
		return "Transenergooil AG";
	case 451:
		return "Bunch";
	case 452:
		return "DME Microelectronics";
	case 453:
		return "Bitcraze AB";
	case 454:
		return "HASWARE Inc.";
	case 455:
		return "Abiogenix Inc.";
	case 456:
		return "Poly-Control ApS";
	case 457:
		return "Avi-on";
	case 458:
		return "Laerdal Medical AS";
	case 459:
		return "Fetch My Pet";
	case 460:
		return "Sam Labs Ltd.";
	case 461:
		return "Chengdu Synwing Technology Ltd";
	case 462:
		return "HOUWA SYSTEM DESIGN, k.k.";
	case 463:
		return "BSH";
	case 464:
		return "Primus Inter Pares Ltd";
	case 465:
		return "August";
	case 466:
		return "Gill Electronics";
	case 467:
		return "Sky Wave Design";
	case 468:
		return "Newlab S.r.l.";
	case 469:
		return "ELAD srl";
	case 470:
		return "G-wearables inc.";
	case 471:
		return "Squadrone Systems Inc.";
	case 472:
		return "Code Corporation";
	case 473:
		return "Savant Systems LLC";
	case 474:
		return "Logitech International SA";
	case 475:
		return "Innblue Consulting";
	case 476:
		return "iParking Ltd.";
	case 477:
		return "Koninklijke Philips Electronics N.V.";
	case 478:
		return "Minelab Electronics Pty Limited";
	case 479:
		return "Bison Group Ltd.";
	case 480:
		return "Widex A/S";
	case 481:
		return "Jolla Ltd";
	case 482:
		return "Lectronix, Inc.";
	case 483:
		return "Caterpillar Inc";
	case 484:
		return "Freedom Innovations";
	case 485:
		return "Dynamic Devices Ltd";
	case 486:
		return "Technology Solutions (UK) Ltd";
	case 487:
		return "IPS Group Inc.";
	case 488:
		return "STIR";
	case 489:
		return "Sano, Inc";
	case 490:
		return "Advanced Application Design, Inc.";
	case 65535:
		return "internal use";
	default:
		return "not assigned";
	}
}
