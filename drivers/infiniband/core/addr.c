/*
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
 * Copyright (c) 1999-2005, Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2005 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/mutex.h>
#include <linux/inetdevice.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <net/arp.h>
#include <net/neighbour.h>
#include <net/route.h>
#include <net/netevent.h>
#include <net/addrconf.h>
#include <net/ip6_route.h>
#include <rdma/ib_addr.h>
#include <rdma/ib.h>

struct addr_req {
	struct list_head list;
	struct sockaddr_storage src_addr;
	struct sockaddr_storage dst_addr;
	struct rdma_dev_addr *addr;
	struct rdma_addr_client *client;
	void *context;
	void (*callback)(int status, struct sockaddr *src_addr,
			 struct rdma_dev_addr *addr, void *context);
	unsigned long timeout;
	int status;
};

static void process_req(struct work_struct *work);

static DEFINE_MUTEX(lock);
static LIST_HEAD(req_list);
static DECLARE_DELAYED_WORK(work, process_req);
static struct workqueue_struct *addr_wq;

int rdma_addr_size(struct sockaddr *addr)
{
	switch (addr->sa_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	case AF_IB:
		return sizeof(struct sockaddr_ib);
	default:
		return 0;
	}
}
EXPORT_SYMBOL(rdma_addr_size);

static struct rdma_addr_client self;

void rdma_addr_register_client(struct rdma_addr_client *client)
{
	atomic_set(&client->refcount, 1);
	init_completion(&client->comp);
}
EXPORT_SYMBOL(rdma_addr_register_client);

static inline void put_client(struct rdma_addr_client *client)
{
	if (atomic_dec_and_test(&client->refcount))
		complete(&client->comp);
}

void rdma_addr_unregister_client(struct rdma_addr_client *client)
{
	put_client(client);
	wait_for_completion(&client->comp);
}
EXPORT_SYMBOL(rdma_addr_unregister_client);

int rdma_copy_addr(struct rdma_dev_addr *dev_addr, struct net_device *dev,
		     const unsigned char *dst_dev_addr)
{
	dev_addr->dev_type = dev->type;
	memcpy(dev_addr->src_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
	memcpy(dev_addr->broadcast, dev->broadcast, MAX_ADDR_LEN);
	if (dst_dev_addr)
		memcpy(dev_addr->dst_dev_addr, dst_dev_addr, MAX_ADDR_LEN);
	dev_addr->bound_dev_if = dev->ifindex;
	return 0;
}
EXPORT_SYMBOL(rdma_copy_addr);

int rdma_translate_ip(const struct sockaddr *addr,
		      struct rdma_dev_addr *dev_addr,
		      u16 *vlan_id)
{
	struct net_device *dev;
	int ret = -EADDRNOTAVAIL;

	if (dev_addr->bound_dev_if) {
		dev = dev_get_by_index(dev_addr->net, dev_addr->bound_dev_if);
		if (!dev)
			return -ENODEV;
		ret = rdma_copy_addr(dev_addr, dev, NULL);
		dev_put(dev);
		return ret;
	}

	switch (addr->sa_family) {
	case AF_INET:
		dev = ip_dev_find(dev_addr->net,
			((const struct sockaddr_in *)addr)->sin_addr.s_addr);

		if (!dev)
			return ret;

		ret = rdma_copy_addr(dev_addr, dev, NULL);
		if (vlan_id)
			*vlan_id = rdma_vlan_dev_vlan_id(dev);
		dev_put(dev);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		rcu_read_lock();
		for_each_netdev_rcu(dev_addr->net, dev) {
			if (ipv6_chk_addr(dev_addr->net,
					  &((const struct sockaddr_in6 *)addr)->sin6_addr,
					  dev, 1)) {
				ret = rdma_copy_addr(dev_addr, dev, NULL);
				if (vlan_id)
					*vlan_id = rdma_vlan_dev_vlan_id(dev);
				break;
			}
		}
		rcu_read_unlock();
		break;
#endif
	}
	return ret;
}
EXPORT_SYMBOL(rdma_translate_ip);

static void set_timeout(unsigned long time)
{
	unsigned long delay;

	delay = time - jiffies;
	if ((long)delay < 0)
		delay = 0;

	mod_delayed_work(addr_wq, &work, delay);
}

static void queue_req(struct addr_req *req)
{
	struct addr_req *temp_req;

	mutex_lock(&lock);
	list_for_each_entry_reverse(temp_req, &req_list, list) {
		if (time_after_eq(req->timeout, temp_req->timeout))
			break;
	}

	list_add(&req->list, &temp_req->list);

	if (req_list.next == &req->list)
		set_timeout(req->timeout);
	mutex_unlock(&lock);
}

static int dst_fetch_ha(struct dst_entry *dst, struct rdma_dev_addr *dev_addr,
			const void *daddr)
{
	struct neighbour *n;
	int ret;

	n = dst_neigh_lookup(dst, daddr);

	rcu_read_lock();
	if (!n || !(n->nud_state & NUD_VALID)) {
		if (n)
			neigh_event_send(n, NULL);
		ret = -ENODATA;
	} else {
		ret = rdma_copy_addr(dev_addr, dst->dev, n->ha);
	}
	rcu_read_unlock();

	if (n)
		neigh_release(n);

	return ret;
}

static int addr4_resolve(struct sockaddr_in *src_in,
			 const struct sockaddr_in *dst_in,
			 struct rdma_dev_addr *addr,
			 struct rtable **prt)
{
	__be32 src_ip = src_in->sin_addr.s_addr;
	__be32 dst_ip = dst_in->sin_addr.s_addr;
	struct rtable *rt;
	struct flowi4 fl4;
	int ret;

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = dst_ip;
	fl4.saddr = src_ip;
	fl4.flowi4_oif = addr->bound_dev_if;
	rt = ip_route_output_key(addr->net, &fl4);
	if (IS_ERR(rt)) {
		ret = PTR_ERR(rt);
		goto out;
	}
	src_in->sin_family = AF_INET;
	src_in->sin_addr.s_addr = fl4.saddr;

	/* If there's a gateway, we're definitely in RoCE v2 (as RoCE v1 isn't
	 * routable) and we could set the network type accordingly.
	 */
	if (rt->rt_uses_gateway)
		addr->network = RDMA_NETWORK_IPV4;

	addr->hoplimit = ip4_dst_hoplimit(&rt->dst);

	*prt = rt;
	return 0;
out:
	return ret;
}

#if IS_ENABLED(CONFIG_IPV6)
static int addr6_resolve(struct sockaddr_in6 *src_in,
			 const struct sockaddr_in6 *dst_in,
			 struct rdma_dev_addr *addr,
			 struct dst_entry **pdst)
{
	struct flowi6 fl6;
	struct dst_entry *dst;
	struct rt6_info *rt;
	int ret;

	memset(&fl6, 0, sizeof fl6);
	fl6.daddr = dst_in->sin6_addr;
	fl6.saddr = src_in->sin6_addr;
	fl6.flowi6_oif = addr->bound_dev_if;

	dst = ip6_route_output(addr->net, NULL, &fl6);
	if ((ret = dst->error))
		goto put;

	rt = (struct rt6_info *)dst;
	if (ipv6_addr_any(&fl6.saddr)) {
		ret = ipv6_dev_get_saddr(addr->net, ip6_dst_idev(dst)->dev,
					 &fl6.daddr, 0, &fl6.saddr);
		if (ret)
			goto put;

		src_in->sin6_family = AF_INET6;
		src_in->sin6_addr = fl6.saddr;
	}

	/* If there's a gateway, we're definitely in RoCE v2 (as RoCE v1 isn't
	 * routable) and we could set the network type accordingly.
	 */
	if (rt->rt6i_flags & RTF_GATEWAY)
		addr->network = RDMA_NETWORK_IPV6;

	addr->hoplimit = ip6_dst_hoplimit(dst);

	*pdst = dst;
	return 0;
put:
	dst_release(dst);
	return ret;
}
#else
static int addr6_resolve(struct sockaddr_in6 *src_in,
			 const struct sockaddr_in6 *dst_in,
			 struct rdma_dev_addr *addr,
			 struct dst_entry **pdst)
{
	return -EADDRNOTAVAIL;
}
#endif

static int addr_resolve_neigh(struct dst_entry *dst,
			      const struct sockaddr *dst_in,
			      struct rdma_dev_addr *addr)
{
	if (dst->dev->flags & IFF_LOOPBACK) {
		int ret;

		ret = rdma_translate_ip(dst_in, addr, NULL);
		if (!ret)
			memcpy(addr->dst_dev_addr, addr->src_dev_addr,
			       MAX_ADDR_LEN);

		return ret;
	}

	/* If the device doesn't do ARP internally */
	if (!(dst->dev->flags & IFF_NOARP)) {
		const struct sockaddr_in *dst_in4 =
			(const struct sockaddr_in *)dst_in;
		const struct sockaddr_in6 *dst_in6 =
			(const struct sockaddr_in6 *)dst_in;

		return dst_fetch_ha(dst, addr,
				    dst_in->sa_family == AF_INET ?
				    (const void *)&dst_in4->sin_addr.s_addr :
				    (const void *)&dst_in6->sin6_addr);
	}

	return rdma_copy_addr(addr, dst->dev, NULL);
}

static int addr_resolve(struct sockaddr *src_in,
			const struct sockaddr *dst_in,
			struct rdma_dev_addr *addr,
			bool resolve_neigh)
{
	struct net_device *ndev;
	struct dst_entry *dst;
	int ret;

	if (src_in->sa_family == AF_INET) {
		struct rtable *rt = NULL;
		const struct sockaddr_in *dst_in4 =
			(const struct sockaddr_in *)dst_in;

		ret = addr4_resolve((struct sockaddr_in *)src_in,
				    dst_in4, addr, &rt);
		if (ret)
			return ret;

		if (resolve_neigh)
			ret = addr_resolve_neigh(&rt->dst, dst_in, addr);

		ndev = rt->dst.dev;
		dev_hold(ndev);

		ip_rt_put(rt);
	} else {
		const struct sockaddr_in6 *dst_in6 =
			(const struct sockaddr_in6 *)dst_in;

		ret = addr6_resolve((struct sockaddr_in6 *)src_in,
				    dst_in6, addr,
				    &dst);
		if (ret)
			return ret;

		if (resolve_neigh)
			ret = addr_resolve_neigh(dst, dst_in, addr);

		ndev = dst->dev;
		dev_hold(ndev);

		dst_release(dst);
	}

	addr->bound_dev_if = ndev->ifindex;
	addr->net = dev_net(ndev);
	dev_put(ndev);

	return ret;
}

static void process_req(struct work_struct *work)
{
	struct addr_req *req, *temp_req;
	struct sockaddr *src_in, *dst_in;
	struct list_head done_list;

	INIT_LIST_HEAD(&done_list);

	mutex_lock(&lock);
	list_for_each_entry_safe(req, temp_req, &req_list, list) {
		if (req->status == -ENODATA) {
			src_in = (struct sockaddr *) &req->src_addr;
			dst_in = (struct sockaddr *) &req->dst_addr;
			req->status = addr_resolve(src_in, dst_in, req->addr,
						   true);
			if (req->status && time_after_eq(jiffies, req->timeout))
				req->status = -ETIMEDOUT;
			else if (req->status == -ENODATA)
				continue;
		}
		list_move_tail(&req->list, &done_list);
	}

	if (!list_empty(&req_list)) {
		req = list_entry(req_list.next, struct addr_req, list);
		set_timeout(req->timeout);
	}
	mutex_unlock(&lock);

	list_for_each_entry_safe(req, temp_req, &done_list, list) {
		list_del(&req->list);
		req->callback(req->status, (struct sockaddr *) &req->src_addr,
			req->addr, req->context);
		put_client(req->client);
		kfree(req);
	}
}

int rdma_resolve_ip(struct rdma_addr_client *client,
		    struct sockaddr *src_addr, struct sockaddr *dst_addr,
		    struct rdma_dev_addr *addr, int timeout_ms,
		    void (*callback)(int status, struct sockaddr *src_addr,
				     struct rdma_dev_addr *addr, void *context),
		    void *context)
{
	struct sockaddr *src_in, *dst_in;
	struct addr_req *req;
	int ret = 0;

	req = kzalloc(sizeof *req, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	src_in = (struct sockaddr *) &req->src_addr;
	dst_in = (struct sockaddr *) &req->dst_addr;

	if (src_addr) {
		if (src_addr->sa_family != dst_addr->sa_family) {
			ret = -EINVAL;
			goto err;
		}

		memcpy(src_in, src_addr, rdma_addr_size(src_addr));
	} else {
		src_in->sa_family = dst_addr->sa_family;
	}

	memcpy(dst_in, dst_addr, rdma_addr_size(dst_addr));
	req->addr = addr;
	req->callback = callback;
	req->context = context;
	req->client = client;
	atomic_inc(&client->refcount);

	req->status = addr_resolve(src_in, dst_in, addr, true);
	switch (req->status) {
	case 0:
		req->timeout = jiffies;
		queue_req(req);
		break;
	case -ENODATA:
		req->timeout = msecs_to_jiffies(timeout_ms) + jiffies;
		queue_req(req);
		break;
	default:
		ret = req->status;
		atomic_dec(&client->refcount);
		goto err;
	}
	return ret;
err:
	kfree(req);
	return ret;
}
EXPORT_SYMBOL(rdma_resolve_ip);

int rdma_resolve_ip_route(struct sockaddr *src_addr,
			  const struct sockaddr *dst_addr,
			  struct rdma_dev_addr *addr)
{
	struct sockaddr_storage ssrc_addr = {};
	struct sockaddr *src_in = (struct sockaddr *)&ssrc_addr;

	if (src_addr) {
		if (src_addr->sa_family != dst_addr->sa_family)
			return -EINVAL;

		memcpy(src_in, src_addr, rdma_addr_size(src_addr));
	} else {
		src_in->sa_family = dst_addr->sa_family;
	}

	return addr_resolve(src_in, dst_addr, addr, false);
}
EXPORT_SYMBOL(rdma_resolve_ip_route);

void rdma_addr_cancel(struct rdma_dev_addr *addr)
{
	struct addr_req *req, *temp_req;

	mutex_lock(&lock);
	list_for_each_entry_safe(req, temp_req, &req_list, list) {
		if (req->addr == addr) {
			req->status = -ECANCELED;
			req->timeout = jiffies;
			list_move(&req->list, &req_list);
			set_timeout(req->timeout);
			break;
		}
	}
	mutex_unlock(&lock);
}
EXPORT_SYMBOL(rdma_addr_cancel);

struct resolve_cb_context {
	struct rdma_dev_addr *addr;
	struct completion comp;
	int status;
};

static void resolve_cb(int status, struct sockaddr *src_addr,
	     struct rdma_dev_addr *addr, void *context)
{
	if (!status)
		memcpy(((struct resolve_cb_context *)context)->addr,
		       addr, sizeof(struct rdma_dev_addr));
	((struct resolve_cb_context *)context)->status = status;
	complete(&((struct resolve_cb_context *)context)->comp);
}

int rdma_addr_find_l2_eth_by_grh(const union ib_gid *sgid,
				 const union ib_gid *dgid,
				 u8 *dmac, u16 *vlan_id, int *if_index,
				 int *hoplimit)
{
	int ret = 0;
	struct rdma_dev_addr dev_addr;
	struct resolve_cb_context ctx;
	struct net_device *dev;

	union {
		struct sockaddr     _sockaddr;
		struct sockaddr_in  _sockaddr_in;
		struct sockaddr_in6 _sockaddr_in6;
	} sgid_addr, dgid_addr;


	rdma_gid2ip(&sgid_addr._sockaddr, sgid);
	rdma_gid2ip(&dgid_addr._sockaddr, dgid);

	memset(&dev_addr, 0, sizeof(dev_addr));
	if (if_index)
		dev_addr.bound_dev_if = *if_index;
	dev_addr.net = &init_net;

	ctx.addr = &dev_addr;
	init_completion(&ctx.comp);
	ret = rdma_resolve_ip(&self, &sgid_addr._sockaddr, &dgid_addr._sockaddr,
			&dev_addr, 1000, resolve_cb, &ctx);
	if (ret)
		return ret;

	wait_for_completion(&ctx.comp);

	ret = ctx.status;
	if (ret)
		return ret;

	memcpy(dmac, dev_addr.dst_dev_addr, ETH_ALEN);
	dev = dev_get_by_index(&init_net, dev_addr.bound_dev_if);
	if (!dev)
		return -ENODEV;
	if (if_index)
		*if_index = dev_addr.bound_dev_if;
	if (vlan_id)
		*vlan_id = rdma_vlan_dev_vlan_id(dev);
	if (hoplimit)
		*hoplimit = dev_addr.hoplimit;
	dev_put(dev);
	return ret;
}
EXPORT_SYMBOL(rdma_addr_find_l2_eth_by_grh);

int rdma_addr_find_smac_by_sgid(union ib_gid *sgid, u8 *smac, u16 *vlan_id)
{
	int ret = 0;
	struct rdma_dev_addr dev_addr;
	union {
		struct sockaddr     _sockaddr;
		struct sockaddr_in  _sockaddr_in;
		struct sockaddr_in6 _sockaddr_in6;
	} gid_addr;

	rdma_gid2ip(&gid_addr._sockaddr, sgid);

	memset(&dev_addr, 0, sizeof(dev_addr));
	dev_addr.net = &init_net;
	ret = rdma_translate_ip(&gid_addr._sockaddr, &dev_addr, vlan_id);
	if (ret)
		return ret;

	memcpy(smac, dev_addr.src_dev_addr, ETH_ALEN);
	return ret;
}
EXPORT_SYMBOL(rdma_addr_find_smac_by_sgid);

static int netevent_callback(struct notifier_block *self, unsigned long event,
	void *ctx)
{
	if (event == NETEVENT_NEIGH_UPDATE) {
		struct neighbour *neigh = ctx;

		if (neigh->nud_state & NUD_VALID) {
			set_timeout(jiffies);
		}
	}
	return 0;
}

static struct notifier_block nb = {
	.notifier_call = netevent_callback
};

int addr_init(void)
{
	addr_wq = alloc_workqueue("ib_addr", WQ_MEM_RECLAIM, 0);
	if (!addr_wq)
		return -ENOMEM;

	register_netevent_notifier(&nb);
	rdma_addr_register_client(&self);
	return 0;
}

void addr_cleanup(void)
{
	rdma_addr_unregister_client(&self);
	unregister_netevent_notifier(&nb);
	destroy_workqueue(addr_wq);
}
