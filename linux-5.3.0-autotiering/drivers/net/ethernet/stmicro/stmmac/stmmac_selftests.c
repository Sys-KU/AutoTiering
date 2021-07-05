// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 Synopsys, Inc. and/or its affiliates.
 * stmmac Selftests Support
 *
 * Author: Jose Abreu <joabreu@synopsys.com>
 */

#include <linux/completion.h>
#include <linux/ethtool.h>
#include <linux/ip.h>
#include <linux/phy.h>
#include <linux/udp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include "stmmac.h"

struct stmmachdr {
	__be32 version;
	__be64 magic;
	u8 id;
} __packed;

#define STMMAC_TEST_PKT_SIZE (sizeof(struct ethhdr) + sizeof(struct iphdr) + \
			      sizeof(struct stmmachdr))
#define STMMAC_TEST_PKT_MAGIC	0xdeadcafecafedeadULL
#define STMMAC_LB_TIMEOUT	msecs_to_jiffies(200)

struct stmmac_packet_attrs {
	int vlan;
	int vlan_id_in;
	int vlan_id_out;
	unsigned char *src;
	unsigned char *dst;
	u32 ip_src;
	u32 ip_dst;
	int tcp;
	int sport;
	int dport;
	u32 exp_hash;
	int dont_wait;
	int timeout;
	int size;
	int remove_sa;
	u8 id;
};

static u8 stmmac_test_next_id;

static struct sk_buff *stmmac_test_get_udp_skb(struct stmmac_priv *priv,
					       struct stmmac_packet_attrs *attr)
{
	struct sk_buff *skb = NULL;
	struct udphdr *uhdr = NULL;
	struct tcphdr *thdr = NULL;
	struct stmmachdr *shdr;
	struct ethhdr *ehdr;
	struct iphdr *ihdr;
	int iplen, size;

	size = attr->size + STMMAC_TEST_PKT_SIZE;
	if (attr->vlan) {
		size += 4;
		if (attr->vlan > 1)
			size += 4;
	}

	if (attr->tcp)
		size += sizeof(struct tcphdr);
	else
		size += sizeof(struct udphdr);

	skb = netdev_alloc_skb(priv->dev, size);
	if (!skb)
		return NULL;

	prefetchw(skb->data);
	skb_reserve(skb, NET_IP_ALIGN);

	if (attr->vlan > 1)
		ehdr = skb_push(skb, ETH_HLEN + 8);
	else if (attr->vlan)
		ehdr = skb_push(skb, ETH_HLEN + 4);
	else if (attr->remove_sa)
		ehdr = skb_push(skb, ETH_HLEN - 6);
	else
		ehdr = skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);

	skb_set_network_header(skb, skb->len);
	ihdr = skb_put(skb, sizeof(*ihdr));

	skb_set_transport_header(skb, skb->len);
	if (attr->tcp)
		thdr = skb_put(skb, sizeof(*thdr));
	else
		uhdr = skb_put(skb, sizeof(*uhdr));

	if (!attr->remove_sa)
		eth_zero_addr(ehdr->h_source);
	eth_zero_addr(ehdr->h_dest);
	if (attr->src && !attr->remove_sa)
		ether_addr_copy(ehdr->h_source, attr->src);
	if (attr->dst)
		ether_addr_copy(ehdr->h_dest, attr->dst);

	if (!attr->remove_sa) {
		ehdr->h_proto = htons(ETH_P_IP);
	} else {
		__be16 *ptr = (__be16 *)ehdr;

		/* HACK */
		ptr[3] = htons(ETH_P_IP);
	}

	if (attr->vlan) {
		__be16 *tag, *proto;

		if (!attr->remove_sa) {
			tag = (void *)ehdr + ETH_HLEN;
			proto = (void *)ehdr + (2 * ETH_ALEN);
		} else {
			tag = (void *)ehdr + ETH_HLEN - 6;
			proto = (void *)ehdr + ETH_ALEN;
		}

		proto[0] = htons(ETH_P_8021Q);
		tag[0] = htons(attr->vlan_id_out);
		tag[1] = htons(ETH_P_IP);
		if (attr->vlan > 1) {
			proto[0] = htons(ETH_P_8021AD);
			tag[1] = htons(ETH_P_8021Q);
			tag[2] = htons(attr->vlan_id_in);
			tag[3] = htons(ETH_P_IP);
		}
	}

	if (attr->tcp) {
		thdr->source = htons(attr->sport);
		thdr->dest = htons(attr->dport);
		thdr->doff = sizeof(struct tcphdr) / 4;
		thdr->check = 0;
	} else {
		uhdr->source = htons(attr->sport);
		uhdr->dest = htons(attr->dport);
		uhdr->len = htons(sizeof(*shdr) + sizeof(*uhdr) + attr->size);
		uhdr->check = 0;
	}

	ihdr->ihl = 5;
	ihdr->ttl = 32;
	ihdr->version = 4;
	if (attr->tcp)
		ihdr->protocol = IPPROTO_TCP;
	else
		ihdr->protocol = IPPROTO_UDP;
	iplen = sizeof(*ihdr) + sizeof(*shdr) + attr->size;
	if (attr->tcp)
		iplen += sizeof(*thdr);
	else
		iplen += sizeof(*uhdr);
	ihdr->tot_len = htons(iplen);
	ihdr->frag_off = 0;
	ihdr->saddr = 0;
	ihdr->daddr = htonl(attr->ip_dst);
	ihdr->tos = 0;
	ihdr->id = 0;
	ip_send_check(ihdr);

	shdr = skb_put(skb, sizeof(*shdr));
	shdr->version = 0;
	shdr->magic = cpu_to_be64(STMMAC_TEST_PKT_MAGIC);
	attr->id = stmmac_test_next_id;
	shdr->id = stmmac_test_next_id++;

	if (attr->size)
		skb_put(skb, attr->size);

	skb->csum = 0;
	skb->ip_summed = CHECKSUM_PARTIAL;
	if (attr->tcp) {
		thdr->check = ~tcp_v4_check(skb->len, ihdr->saddr, ihdr->daddr, 0);
		skb->csum_start = skb_transport_header(skb) - skb->head;
		skb->csum_offset = offsetof(struct tcphdr, check);
	} else {
		udp4_hwcsum(skb, ihdr->saddr, ihdr->daddr);
	}

	skb->protocol = htons(ETH_P_IP);
	skb->pkt_type = PACKET_HOST;
	skb->dev = priv->dev;

	return skb;
}

struct stmmac_test_priv {
	struct stmmac_packet_attrs *packet;
	struct packet_type pt;
	struct completion comp;
	int double_vlan;
	int vlan_id;
	int ok;
};

static int stmmac_test_loopback_validate(struct sk_buff *skb,
					 struct net_device *ndev,
					 struct packet_type *pt,
					 struct net_device *orig_ndev)
{
	struct stmmac_test_priv *tpriv = pt->af_packet_priv;
	struct stmmachdr *shdr;
	struct ethhdr *ehdr;
	struct udphdr *uhdr;
	struct tcphdr *thdr;
	struct iphdr *ihdr;

	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb)
		goto out;

	if (skb_linearize(skb))
		goto out;
	if (skb_headlen(skb) < (STMMAC_TEST_PKT_SIZE - ETH_HLEN))
		goto out;

	ehdr = (struct ethhdr *)skb_mac_header(skb);
	if (tpriv->packet->dst) {
		if (!ether_addr_equal(ehdr->h_dest, tpriv->packet->dst))
			goto out;
	}
	if (tpriv->packet->src) {
		if (!ether_addr_equal(ehdr->h_source, orig_ndev->dev_addr))
			goto out;
	}

	ihdr = ip_hdr(skb);
	if (tpriv->double_vlan)
		ihdr = (struct iphdr *)(skb_network_header(skb) + 4);

	if (tpriv->packet->tcp) {
		if (ihdr->protocol != IPPROTO_TCP)
			goto out;

		thdr = (struct tcphdr *)((u8 *)ihdr + 4 * ihdr->ihl);
		if (thdr->dest != htons(tpriv->packet->dport))
			goto out;

		shdr = (struct stmmachdr *)((u8 *)thdr + sizeof(*thdr));
	} else {
		if (ihdr->protocol != IPPROTO_UDP)
			goto out;

		uhdr = (struct udphdr *)((u8 *)ihdr + 4 * ihdr->ihl);
		if (uhdr->dest != htons(tpriv->packet->dport))
			goto out;

		shdr = (struct stmmachdr *)((u8 *)uhdr + sizeof(*uhdr));
	}

	if (shdr->magic != cpu_to_be64(STMMAC_TEST_PKT_MAGIC))
		goto out;
	if (tpriv->packet->exp_hash && !skb->hash)
		goto out;
	if (tpriv->packet->id != shdr->id)
		goto out;

	tpriv->ok = true;
	complete(&tpriv->comp);
out:
	kfree_skb(skb);
	return 0;
}

static int __stmmac_test_loopback(struct stmmac_priv *priv,
				  struct stmmac_packet_attrs *attr)
{
	struct stmmac_test_priv *tpriv;
	struct sk_buff *skb = NULL;
	int ret = 0;

	tpriv = kzalloc(sizeof(*tpriv), GFP_KERNEL);
	if (!tpriv)
		return -ENOMEM;

	tpriv->ok = false;
	init_completion(&tpriv->comp);

	tpriv->pt.type = htons(ETH_P_IP);
	tpriv->pt.func = stmmac_test_loopback_validate;
	tpriv->pt.dev = priv->dev;
	tpriv->pt.af_packet_priv = tpriv;
	tpriv->packet = attr;
	dev_add_pack(&tpriv->pt);

	skb = stmmac_test_get_udp_skb(priv, attr);
	if (!skb) {
		ret = -ENOMEM;
		goto cleanup;
	}

	skb_set_queue_mapping(skb, 0);
	ret = dev_queue_xmit(skb);
	if (ret)
		goto cleanup;

	if (attr->dont_wait)
		goto cleanup;

	if (!attr->timeout)
		attr->timeout = STMMAC_LB_TIMEOUT;

	wait_for_completion_timeout(&tpriv->comp, attr->timeout);
	ret = !tpriv->ok;

cleanup:
	dev_remove_pack(&tpriv->pt);
	kfree(tpriv);
	return ret;
}

static int stmmac_test_mac_loopback(struct stmmac_priv *priv)
{
	struct stmmac_packet_attrs attr = { };

	attr.dst = priv->dev->dev_addr;
	return __stmmac_test_loopback(priv, &attr);
}

static int stmmac_test_phy_loopback(struct stmmac_priv *priv)
{
	struct stmmac_packet_attrs attr = { };
	int ret;

	if (!priv->dev->phydev)
		return -EBUSY;

	ret = phy_loopback(priv->dev->phydev, true);
	if (ret)
		return ret;

	attr.dst = priv->dev->dev_addr;
	ret = __stmmac_test_loopback(priv, &attr);

	phy_loopback(priv->dev->phydev, false);
	return ret;
}

static int stmmac_test_mmc(struct stmmac_priv *priv)
{
	struct stmmac_counters initial, final;
	int ret;

	memset(&initial, 0, sizeof(initial));
	memset(&final, 0, sizeof(final));

	if (!priv->dma_cap.rmon)
		return -EOPNOTSUPP;

	/* Save previous results into internal struct */
	stmmac_mmc_read(priv, priv->mmcaddr, &priv->mmc);

	ret = stmmac_test_mac_loopback(priv);
	if (ret)
		return ret;

	/* These will be loopback results so no need to save them */
	stmmac_mmc_read(priv, priv->mmcaddr, &final);

	/*
	 * The number of MMC counters available depends on HW configuration
	 * so we just use this one to validate the feature. I hope there is
	 * not a version without this counter.
	 */
	if (final.mmc_tx_framecount_g <= initial.mmc_tx_framecount_g)
		return -EINVAL;

	return 0;
}

static int stmmac_test_eee(struct stmmac_priv *priv)
{
	struct stmmac_extra_stats *initial, *final;
	int retries = 10;
	int ret;

	if (!priv->dma_cap.eee || !priv->eee_active)
		return -EOPNOTSUPP;

	initial = kzalloc(sizeof(*initial), GFP_KERNEL);
	if (!initial)
		return -ENOMEM;

	final = kzalloc(sizeof(*final), GFP_KERNEL);
	if (!final) {
		ret = -ENOMEM;
		goto out_free_initial;
	}

	memcpy(initial, &priv->xstats, sizeof(*initial));

	ret = stmmac_test_mac_loopback(priv);
	if (ret)
		goto out_free_final;

	/* We have no traffic in the line so, sooner or later it will go LPI */
	while (--retries) {
		memcpy(final, &priv->xstats, sizeof(*final));

		if (final->irq_tx_path_in_lpi_mode_n >
		    initial->irq_tx_path_in_lpi_mode_n)
			break;
		msleep(100);
	}

	if (!retries) {
		ret = -ETIMEDOUT;
		goto out_free_final;
	}

	if (final->irq_tx_path_in_lpi_mode_n <=
	    initial->irq_tx_path_in_lpi_mode_n) {
		ret = -EINVAL;
		goto out_free_final;
	}

	if (final->irq_tx_path_exit_lpi_mode_n <=
	    initial->irq_tx_path_exit_lpi_mode_n) {
		ret = -EINVAL;
		goto out_free_final;
	}

out_free_final:
	kfree(final);
out_free_initial:
	kfree(initial);
	return ret;
}

static int stmmac_filter_check(struct stmmac_priv *priv)
{
	if (!(priv->dev->flags & IFF_PROMISC))
		return 0;

	netdev_warn(priv->dev, "Test can't be run in promiscuous mode!\n");
	return -EOPNOTSUPP;
}

static int stmmac_test_hfilt(struct stmmac_priv *priv)
{
	unsigned char gd_addr[ETH_ALEN] = {0x01, 0x00, 0xcc, 0xcc, 0xdd, 0xdd};
	unsigned char bd_addr[ETH_ALEN] = {0x09, 0x00, 0xaa, 0xaa, 0xbb, 0xbb};
	struct stmmac_packet_attrs attr = { };
	int ret;

	ret = stmmac_filter_check(priv);
	if (ret)
		return ret;

	ret = dev_mc_add(priv->dev, gd_addr);
	if (ret)
		return ret;

	attr.dst = gd_addr;

	/* Shall receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	if (ret)
		goto cleanup;

	attr.dst = bd_addr;

	/* Shall NOT receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	ret = !ret;

cleanup:
	dev_mc_del(priv->dev, gd_addr);
	return ret;
}

static int stmmac_test_pfilt(struct stmmac_priv *priv)
{
	unsigned char gd_addr[ETH_ALEN] = {0x00, 0x01, 0x44, 0x55, 0x66, 0x77};
	unsigned char bd_addr[ETH_ALEN] = {0x08, 0x00, 0x22, 0x33, 0x44, 0x55};
	struct stmmac_packet_attrs attr = { };
	int ret;

	if (stmmac_filter_check(priv))
		return -EOPNOTSUPP;

	ret = dev_uc_add(priv->dev, gd_addr);
	if (ret)
		return ret;

	attr.dst = gd_addr;

	/* Shall receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	if (ret)
		goto cleanup;

	attr.dst = bd_addr;

	/* Shall NOT receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	ret = !ret;

cleanup:
	dev_uc_del(priv->dev, gd_addr);
	return ret;
}

static int stmmac_dummy_sync(struct net_device *netdev, const u8 *addr)
{
	return 0;
}

static void stmmac_test_set_rx_mode(struct net_device *netdev)
{
	/* As we are in test mode of ethtool we already own the rtnl lock
	 * so no address will change from user. We can just call the
	 * ndo_set_rx_mode() callback directly */
	if (netdev->netdev_ops->ndo_set_rx_mode)
		netdev->netdev_ops->ndo_set_rx_mode(netdev);
}

static int stmmac_test_mcfilt(struct stmmac_priv *priv)
{
	unsigned char uc_addr[ETH_ALEN] = {0x00, 0x01, 0x44, 0x55, 0x66, 0x77};
	unsigned char mc_addr[ETH_ALEN] = {0x01, 0x01, 0x44, 0x55, 0x66, 0x77};
	struct stmmac_packet_attrs attr = { };
	int ret;

	if (stmmac_filter_check(priv))
		return -EOPNOTSUPP;

	/* Remove all MC addresses */
	__dev_mc_unsync(priv->dev, NULL);
	stmmac_test_set_rx_mode(priv->dev);

	ret = dev_uc_add(priv->dev, uc_addr);
	if (ret)
		goto cleanup;

	attr.dst = uc_addr;

	/* Shall receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	if (ret)
		goto cleanup;

	attr.dst = mc_addr;

	/* Shall NOT receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	ret = !ret;

cleanup:
	dev_uc_del(priv->dev, uc_addr);
	__dev_mc_sync(priv->dev, stmmac_dummy_sync, NULL);
	stmmac_test_set_rx_mode(priv->dev);
	return ret;
}

static int stmmac_test_ucfilt(struct stmmac_priv *priv)
{
	unsigned char uc_addr[ETH_ALEN] = {0x00, 0x01, 0x44, 0x55, 0x66, 0x77};
	unsigned char mc_addr[ETH_ALEN] = {0x01, 0x01, 0x44, 0x55, 0x66, 0x77};
	struct stmmac_packet_attrs attr = { };
	int ret;

	if (stmmac_filter_check(priv))
		return -EOPNOTSUPP;

	/* Remove all UC addresses */
	__dev_uc_unsync(priv->dev, NULL);
	stmmac_test_set_rx_mode(priv->dev);

	ret = dev_mc_add(priv->dev, mc_addr);
	if (ret)
		goto cleanup;

	attr.dst = mc_addr;

	/* Shall receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	if (ret)
		goto cleanup;

	attr.dst = uc_addr;

	/* Shall NOT receive packet */
	ret = __stmmac_test_loopback(priv, &attr);
	ret = !ret;

cleanup:
	dev_mc_del(priv->dev, mc_addr);
	__dev_uc_sync(priv->dev, stmmac_dummy_sync, NULL);
	stmmac_test_set_rx_mode(priv->dev);
	return ret;
}

static int stmmac_test_flowctrl_validate(struct sk_buff *skb,
					 struct net_device *ndev,
					 struct packet_type *pt,
					 struct net_device *orig_ndev)
{
	struct stmmac_test_priv *tpriv = pt->af_packet_priv;
	struct ethhdr *ehdr;

	ehdr = (struct ethhdr *)skb_mac_header(skb);
	if (!ether_addr_equal(ehdr->h_source, orig_ndev->dev_addr))
		goto out;
	if (ehdr->h_proto != htons(ETH_P_PAUSE))
		goto out;

	tpriv->ok = true;
	complete(&tpriv->comp);
out:
	kfree_skb(skb);
	return 0;
}

static int stmmac_test_flowctrl(struct stmmac_priv *priv)
{
	unsigned char paddr[ETH_ALEN] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x01};
	struct phy_device *phydev = priv->dev->phydev;
	u32 rx_cnt = priv->plat->rx_queues_to_use;
	struct stmmac_test_priv *tpriv;
	unsigned int pkt_count;
	int i, ret = 0;

	if (!phydev || !phydev->pause)
		return -EOPNOTSUPP;

	tpriv = kzalloc(sizeof(*tpriv), GFP_KERNEL);
	if (!tpriv)
		return -ENOMEM;

	tpriv->ok = false;
	init_completion(&tpriv->comp);
	tpriv->pt.type = htons(ETH_P_PAUSE);
	tpriv->pt.func = stmmac_test_flowctrl_validate;
	tpriv->pt.dev = priv->dev;
	tpriv->pt.af_packet_priv = tpriv;
	dev_add_pack(&tpriv->pt);

	/* Compute minimum number of packets to make FIFO full */
	pkt_count = priv->plat->rx_fifo_size;
	if (!pkt_count)
		pkt_count = priv->dma_cap.rx_fifo_size;
	pkt_count /= 1400;
	pkt_count *= 2;

	for (i = 0; i < rx_cnt; i++)
		stmmac_stop_rx(priv, priv->ioaddr, i);

	ret = dev_set_promiscuity(priv->dev, 1);
	if (ret)
		goto cleanup;

	ret = dev_mc_add(priv->dev, paddr);
	if (ret)
		goto cleanup;

	for (i = 0; i < pkt_count; i++) {
		struct stmmac_packet_attrs attr = { };

		attr.dst = priv->dev->dev_addr;
		attr.dont_wait = true;
		attr.size = 1400;

		ret = __stmmac_test_loopback(priv, &attr);
		if (ret)
			goto cleanup;
		if (tpriv->ok)
			break;
	}

	/* Wait for some time in case RX Watchdog is enabled */
	msleep(200);

	for (i = 0; i < rx_cnt; i++) {
		struct stmmac_channel *ch = &priv->channel[i];

		stmmac_start_rx(priv, priv->ioaddr, i);
		local_bh_disable();
		napi_reschedule(&ch->rx_napi);
		local_bh_enable();
	}

	wait_for_completion_timeout(&tpriv->comp, STMMAC_LB_TIMEOUT);
	ret = !tpriv->ok;

cleanup:
	dev_mc_del(priv->dev, paddr);
	dev_set_promiscuity(priv->dev, -1);
	dev_remove_pack(&tpriv->pt);
	kfree(tpriv);
	return ret;
}

#define STMMAC_LOOPBACK_NONE	0
#define STMMAC_LOOPBACK_MAC	1
#define STMMAC_LOOPBACK_PHY	2

static const struct stmmac_test {
	char name[ETH_GSTRING_LEN];
	int lb;
	int (*fn)(struct stmmac_priv *priv);
} stmmac_selftests[] = {
	{
		.name = "MAC Loopback         ",
		.lb = STMMAC_LOOPBACK_MAC,
		.fn = stmmac_test_mac_loopback,
	}, {
		.name = "PHY Loopback         ",
		.lb = STMMAC_LOOPBACK_NONE, /* Test will handle it */
		.fn = stmmac_test_phy_loopback,
	}, {
		.name = "MMC Counters         ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_mmc,
	}, {
		.name = "EEE                  ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_eee,
	}, {
		.name = "Hash Filter MC       ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_hfilt,
	}, {
		.name = "Perfect Filter UC    ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_pfilt,
	}, {
		.name = "MC Filter            ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_mcfilt,
	}, {
		.name = "UC Filter            ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_ucfilt,
	}, {
		.name = "Flow Control         ",
		.lb = STMMAC_LOOPBACK_PHY,
		.fn = stmmac_test_flowctrl,
	},
};

void stmmac_selftest_run(struct net_device *dev,
			 struct ethtool_test *etest, u64 *buf)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int count = stmmac_selftest_get_count(priv);
	int carrier = netif_carrier_ok(dev);
	int i, ret;

	memset(buf, 0, sizeof(*buf) * count);
	stmmac_test_next_id = 0;

	if (etest->flags != ETH_TEST_FL_OFFLINE) {
		netdev_err(priv->dev, "Only offline tests are supported\n");
		etest->flags |= ETH_TEST_FL_FAILED;
		return;
	} else if (!carrier) {
		netdev_err(priv->dev, "You need valid Link to execute tests\n");
		etest->flags |= ETH_TEST_FL_FAILED;
		return;
	}

	/* We don't want extra traffic */
	netif_carrier_off(dev);

	/* Wait for queues drain */
	msleep(200);

	for (i = 0; i < count; i++) {
		ret = 0;

		switch (stmmac_selftests[i].lb) {
		case STMMAC_LOOPBACK_PHY:
			ret = -EOPNOTSUPP;
			if (dev->phydev)
				ret = phy_loopback(dev->phydev, true);
			if (!ret)
				break;
			/* Fallthrough */
		case STMMAC_LOOPBACK_MAC:
			ret = stmmac_set_mac_loopback(priv, priv->ioaddr, true);
			break;
		case STMMAC_LOOPBACK_NONE:
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}

		/*
		 * First tests will always be MAC / PHY loobpack. If any of
		 * them is not supported we abort earlier.
		 */
		if (ret) {
			netdev_err(priv->dev, "Loopback is not supported\n");
			etest->flags |= ETH_TEST_FL_FAILED;
			break;
		}

		ret = stmmac_selftests[i].fn(priv);
		if (ret && (ret != -EOPNOTSUPP))
			etest->flags |= ETH_TEST_FL_FAILED;
		buf[i] = ret;

		switch (stmmac_selftests[i].lb) {
		case STMMAC_LOOPBACK_PHY:
			ret = -EOPNOTSUPP;
			if (dev->phydev)
				ret = phy_loopback(dev->phydev, false);
			if (!ret)
				break;
			/* Fallthrough */
		case STMMAC_LOOPBACK_MAC:
			stmmac_set_mac_loopback(priv, priv->ioaddr, false);
			break;
		default:
			break;
		}
	}

	/* Restart everything */
	if (carrier)
		netif_carrier_on(dev);
}

void stmmac_selftest_get_strings(struct stmmac_priv *priv, u8 *data)
{
	u8 *p = data;
	int i;

	for (i = 0; i < stmmac_selftest_get_count(priv); i++) {
		snprintf(p, ETH_GSTRING_LEN, "%2d. %s", i + 1,
			 stmmac_selftests[i].name);
		p += ETH_GSTRING_LEN;
	}
}

int stmmac_selftest_get_count(struct stmmac_priv *priv)
{
	return ARRAY_SIZE(stmmac_selftests);
}
