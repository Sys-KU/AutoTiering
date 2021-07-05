// SPDX-License-Identifier: ISC
/* Copyright (C) 2019 MediaTek Inc.
 *
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *         Felix Fietkau <nbd@nbd.name>
 */

#include "mt7615.h"
#include "eeprom.h"

static int mt7615_efuse_read(struct mt7615_dev *dev, u32 base,
			     u16 addr, u8 *data)
{
	u32 val;
	int i;

	val = mt76_rr(dev, base + MT_EFUSE_CTRL);
	val &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
	val |= FIELD_PREP(MT_EFUSE_CTRL_AIN, addr & ~0xf);
	val |= MT_EFUSE_CTRL_KICK;
	mt76_wr(dev, base + MT_EFUSE_CTRL, val);

	if (!mt76_poll(dev, base + MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0, 1000))
		return -ETIMEDOUT;

	udelay(2);

	val = mt76_rr(dev, base + MT_EFUSE_CTRL);
	if ((val & MT_EFUSE_CTRL_AOUT) == MT_EFUSE_CTRL_AOUT ||
	    WARN_ON_ONCE(!(val & MT_EFUSE_CTRL_VALID))) {
		memset(data, 0x0, 16);
		return 0;
	}

	for (i = 0; i < 4; i++) {
		val = mt76_rr(dev, base + MT_EFUSE_RDATA(i));
		put_unaligned_le32(val, data + 4 * i);
	}

	return 0;
}

static int mt7615_efuse_init(struct mt7615_dev *dev)
{
	u32 val, base = mt7615_reg_map(dev, MT_EFUSE_BASE);
	int i, len = MT7615_EEPROM_SIZE;
	void *buf;

	val = mt76_rr(dev, base + MT_EFUSE_BASE_CTRL);
	if (val & MT_EFUSE_BASE_CTRL_EMPTY)
		return 0;

	dev->mt76.otp.data = devm_kzalloc(dev->mt76.dev, len, GFP_KERNEL);
	dev->mt76.otp.size = len;
	if (!dev->mt76.otp.data)
		return -ENOMEM;

	buf = dev->mt76.otp.data;
	for (i = 0; i + 16 <= len; i += 16) {
		int ret;

		ret = mt7615_efuse_read(dev, base, i, buf + i);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt7615_eeprom_load(struct mt7615_dev *dev)
{
	int ret;

	ret = mt76_eeprom_init(&dev->mt76, MT7615_EEPROM_SIZE);
	if (ret < 0)
		return ret;

	return mt7615_efuse_init(dev);
}

static int mt7615_check_eeprom(struct mt76_dev *dev)
{
	u16 val = get_unaligned_le16(dev->eeprom.data);

	switch (val) {
	case 0x7615:
		return 0;
	default:
		return -EINVAL;
	}
}

static void mt7615_eeprom_parse_hw_cap(struct mt7615_dev *dev)
{
	u8 val, *eeprom = dev->mt76.eeprom.data;

	val = FIELD_GET(MT_EE_NIC_WIFI_CONF_BAND_SEL,
			eeprom[MT_EE_WIFI_CONF]);
	switch (val) {
	case MT_EE_5GHZ:
		dev->mt76.cap.has_5ghz = true;
		break;
	case MT_EE_2GHZ:
		dev->mt76.cap.has_2ghz = true;
		break;
	default:
		dev->mt76.cap.has_2ghz = true;
		dev->mt76.cap.has_5ghz = true;
		break;
	}
}

int mt7615_eeprom_get_power_index(struct mt7615_dev *dev,
				  struct ieee80211_channel *chan,
				  u8 chain_idx)
{
	int index;

	if (chain_idx > 3)
		return -EINVAL;

	/* TSSI disabled */
	if (mt7615_ext_pa_enabled(dev, chan->band)) {
		if (chan->band == NL80211_BAND_2GHZ)
			return MT_EE_EXT_PA_2G_TARGET_POWER;
		else
			return MT_EE_EXT_PA_5G_TARGET_POWER;
	}

	/* TSSI enabled */
	if (chan->band == NL80211_BAND_2GHZ) {
		index = MT_EE_TX0_2G_TARGET_POWER + chain_idx * 6;
	} else {
		int group = mt7615_get_channel_group(chan->hw_value);

		switch (chain_idx) {
		case 1:
			index = MT_EE_TX1_5G_G0_TARGET_POWER;
			break;
		case 2:
			index = MT_EE_TX2_5G_G0_TARGET_POWER;
			break;
		case 3:
			index = MT_EE_TX3_5G_G0_TARGET_POWER;
			break;
		case 0:
		default:
			index = MT_EE_TX0_5G_G0_TARGET_POWER;
			break;
		}
		index += 5 * group;
	}

	return index;
}

int mt7615_eeprom_init(struct mt7615_dev *dev)
{
	int ret;

	ret = mt7615_eeprom_load(dev);
	if (ret < 0)
		return ret;

	ret = mt7615_check_eeprom(&dev->mt76);
	if (ret && dev->mt76.otp.data)
		memcpy(dev->mt76.eeprom.data, dev->mt76.otp.data,
		       MT7615_EEPROM_SIZE);

	mt7615_eeprom_parse_hw_cap(dev);
	memcpy(dev->mt76.macaddr, dev->mt76.eeprom.data + MT_EE_MAC_ADDR,
	       ETH_ALEN);

	mt76_eeprom_override(&dev->mt76);

	return 0;
}
