/*
 * net/sched/sch_cn.c Fair Queue Packet Scheduler (per flow pacing)
 *
 *  Copyright (C) 2013-2015 Eric Dumazet <edumazet@google.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *  Meant to be mostly used for locally generated traffic :
 *  Fast classification depends on skb->sk being set before reaching us.
 *  If not, (router workload), we use rxhash as fallback, with 32 bits wide hash.
 *  All packets belonging to a socket are considered as a 'flow'.
 *
 *  Flows are dynamically allocated and stored in a hash table of RB trees
 *  They are also part of one Round Robin 'queues' (new or old flows)
 *
 *  Burst avoidance (aka pacing) capability :
 *
 *  Transport (eg TCP) can set in sk->sk_pacing_rate a rate, enqueue a
 *  bunch of packets, and this packet scheduler adds delay between
 *  packets to respect rate limitation.
 *
 *  enqueue() :
 *   - lookup one RB tree (out of 1024 or more) to find the flow.
 *     If non existent flow, create it, add it to the tree.
 *     Add skb to the per flow list of skb (fifo).
 *   - Use a special fifo for high prio packets
 *
 *  dequeue() : serves flows in Round Robin
 *  Note : When a flow becomes empty, we do not immediately remove it from
 *  rb trees, for performance reasons (its expected to send additional packets,
 *  or SLAB cache will reuse socket for another flow)
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/hash.h>
#include <linux/prefetch.h>
#include <linux/vmalloc.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>
#include <linux/limits.h>
#include <asm/fpu/api.h>
#include <linux/timekeeping.h>

// Added by max
#include "iproute2/tc/cn_opts.h"

// How many times the previously longest interval to wait?
// double guard_interval = 0.5;
// double maximum_change = 2.0;

static u64 seconds_from_ns(u64 ns) {
	u64 current_ns = ktime_get_ns();
	return (current_ns-ns)/1000000000;
}

// FIXME: se only u64 because why not. Maybe fix this in the long term...
struct interval_info {
	u64 start_ns;
	u64 end_ns;
	u64 idle_ns;
	u64 min_queue_length; // FIXME: everything only works with packets now and not bytes
	u64 packets_transmitted;
};

struct five_tuple {
	u32 src_ip;
	u32 dst_ip;
	u16 src_port;
	u16 dst_port;
	u8 transport_protocol;
};

/*
 * Per flow structure, dynamically allocated
 */
struct cn_flow {
	struct sk_buff	*head;		/* list of skbs for this flow : first skb */
	union {
		struct sk_buff *tail;	/* last skb in the list */
		unsigned long  age;	/* jiffies when flow was emptied, for gc */
	};
	struct rb_node	cn_node;	/* anchor in cn_root[] trees */
	struct sock	*sk;
	int		qlen;		/* number of packets in flow queue */

	int		flow_max_qlen;

	int		credit;
	u32		socket_hash;	/* sk_hash */
	struct cn_flow *next;		/* next pointer in RR lists, or &detached */
	struct rb_node  rate_node;	/* anchor in q->delayed tree */
	u64		time_next_packet;

	// Added by Max
	u64 flow_start_ns;
	struct five_tuple ft;
	struct interval_info longest_interval;
	struct interval_info current_interval;
	u64 monitoring_period_start_ns;
	u64 monitoring_period_end_ns;
	bool idle;
	u64 became_idle_ns;
	u64 enlarge;
};

struct cn_flow_head {
	struct cn_flow *first;
	struct cn_flow *last;
};

struct cn_sched_data {
	struct cn_flow_head new_flows;

	struct cn_flow_head old_flows;

	struct rb_root	delayed;	/* for rate limited flows */
	u64		time_next_delayed_flow;
	unsigned long	unthrottle_latency_ns;

	struct cn_flow	internal;	/* for non classified or high prio packets */
	u32		quantum;
	u32		initial_quantum;
	u32		flow_refill_delay;
	u32		flow_max_rate;	/* optional max rate per flow */
	u32		flow_plimit;	/* max packets per flow */
	u32		orphan_mask;	/* mask for orphaned skb */
	u32		low_rate_threshold;
	struct rb_root	*cn_root;
	u8		rate_enable;
	u8		cn_trees_log;

	u32		flows;
	u32		inactive_flows;
	u32		throttled_flows;

	u64		stat_gc_flows;
	u64		stat_internal_packets;
	u64		stat_tcp_retrans;
	u64		stat_throttled;
	u64		stat_flows_plimit;
	u64		stat_pkts_too_long;
	u64		stat_allocation_errors;
	struct qdisc_watchdog watchdog;

	// Added by Max;
	double guard_interval;
	double max_increase;
};

struct ipv4_address {
	char bytes[32];
};

struct ipv4_address get_ip(unsigned int ip) {
		unsigned char bytes[4];
		struct ipv4_address ip_str;
		bytes[0] = ip & 0xFF;
		bytes[1] = (ip >> 8) & 0xFF;
		bytes[2] = (ip >> 16) & 0xFF;
		bytes[3] = (ip >> 24) & 0xFF;
		snprintf(ip_str.bytes, sizeof(ip_str.bytes), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
		return ip_str;
}

static struct five_tuple get_five_tuple_from_skb(struct sk_buff *skb) {
	struct iphdr *ip_header = (struct iphdr *)skb_network_header(skb);
	struct udphdr *udp_header;
	struct tcphdr *tcp_header;
	struct five_tuple ft;
	// struct list_head *p;

	unsigned int src_ip = (unsigned int)ip_header->saddr;
	unsigned int dst_ip = (unsigned int)ip_header->daddr;
	unsigned int src_port = 0;
	unsigned int dst_port = 0;

	if (ip_header->protocol==17) {
					udp_header = (struct udphdr *)skb_transport_header(skb);
					src_port = (unsigned int)ntohs(udp_header->source);
	} else if (ip_header->protocol == 6) {
					tcp_header = (struct tcphdr *)skb_transport_header(skb);
					src_port = (unsigned int)ntohs(tcp_header->source);
					dst_port = (unsigned int)ntohs(tcp_header->dest);
	}

	ft.src_ip=src_ip,
	ft.dst_ip=dst_ip;
	ft.src_port=src_port;
	ft.dst_port=dst_port;
	ft.transport_protocol=ip_header->protocol;

	return ft;
}

/* special value to mark a detached flow (not on old/new list) */
static struct cn_flow detached, throttled;

static void cn_flow_set_detached(struct cn_flow *f)
{
	f->next = &detached;
	f->age = jiffies;
}

static bool cn_flow_is_detached(const struct cn_flow *f)
{
	return f->next == &detached;
}

static bool cn_flow_is_throttled(const struct cn_flow *f)
{
	return f->next == &throttled;
}

static void cn_flow_add_tail(struct cn_flow_head *head, struct cn_flow *flow)
{
	if (head->first)
		head->last->next = flow;
	else
		head->first = flow;
	head->last = flow;
	flow->next = NULL;
}

static void cn_flow_unset_throttled(struct cn_sched_data *q, struct cn_flow *f)
{
	rb_erase(&f->rate_node, &q->delayed);
	q->throttled_flows--;
	cn_flow_add_tail(&q->old_flows, f);
}

static void cn_flow_set_throttled(struct cn_sched_data *q, struct cn_flow *f)
{
	struct rb_node **p = &q->delayed.rb_node, *parent = NULL;

	while (*p) {
		struct cn_flow *aux;

		parent = *p;
		aux = rb_entry(parent, struct cn_flow, rate_node);
		if (f->time_next_packet >= aux->time_next_packet)
			p = &parent->rb_right;
		else
			p = &parent->rb_left;
	}
	rb_link_node(&f->rate_node, parent, p);
	rb_insert_color(&f->rate_node, &q->delayed);
	q->throttled_flows++;
	q->stat_throttled++;

	f->next = &throttled;
	if (q->time_next_delayed_flow > f->time_next_packet)
		q->time_next_delayed_flow = f->time_next_packet;
}

// Created by Max
static void cn_copy_longest_interval_if_needed(struct cn_flow *f) {
	if ((f->current_interval.end_ns - f->current_interval.start_ns) >= (f->longest_interval.end_ns - f->longest_interval.start_ns)) {
		f->longest_interval = f->current_interval;
	}
}

static struct kmem_cache *cn_flow_cachep __read_mostly;

#define FLOW_TIMEOUT 60

/* limit number of collected flows per round */
#define CN_GC_MAX 8
// Changed by Max. Shouldn't be needed though because only when there are sufficient
// flows GC kicks in
// #define CN_GC_AGE (3*HZ)
#define CN_GC_AGE (FLOW_TIMEOUT*HZ)

static bool cn_gc_candidate(const struct cn_flow *f)
{
	return cn_flow_is_detached(f) &&
				 time_after(jiffies, f->age + CN_GC_AGE);
}

static void cn_gc(struct cn_sched_data *q,
			struct rb_root *root,
			struct sock *sk)
{
	struct cn_flow *f, *tofree[CN_GC_MAX];
	struct rb_node **p, *parent;
	int fcnt = 0;

	p = &root->rb_node;
	parent = NULL;
	while (*p) {
		parent = *p;

		f = rb_entry(parent, struct cn_flow, cn_node);
		if (f->sk == sk)
			break;

		if (cn_gc_candidate(f)) {
			tofree[fcnt++] = f;
			if (fcnt == CN_GC_MAX)
				break;
		}

		if (f->sk > sk)
			p = &parent->rb_right;
		else
			p = &parent->rb_left;
	}

	q->flows -= fcnt;
	q->inactive_flows -= fcnt;
	q->stat_gc_flows += fcnt;
	while (fcnt) {
		struct cn_flow *f = tofree[--fcnt];
		trace_printk("sch_cn: At %llu, Garbage collected %u flows!\n", seconds_from_ns(f->flow_start_ns), (u32) fcnt);

		rb_erase(&f->cn_node, root);
		kmem_cache_free(cn_flow_cachep, f);
	}
}

static void cn_initialize_interval(struct cn_flow *f) {
	u64 current_ns = ktime_get_ns();
	f->current_interval.start_ns = current_ns;
	f->current_interval.end_ns = current_ns;
	f->current_interval.idle_ns = 0;
	f->current_interval.min_queue_length = ULONG_MAX;
	f->current_interval.packets_transmitted = 0;

	cn_copy_longest_interval_if_needed(f);
}

static void cn_initialize_monitoring_interval(struct cn_flow *f) {
	u64 current_ns = ktime_get_ns();
	f->monitoring_period_start_ns = current_ns;
	f->monitoring_period_end_ns = current_ns;
	f->became_idle_ns = 0;
	f->idle = false;
}

static struct cn_flow *cn_classify(struct sk_buff *skb, struct cn_sched_data *q)
{
	struct rb_node **p, *parent;
	struct sock *sk = skb->sk;
	struct rb_root *root;
	struct cn_flow *f;
	struct five_tuple ft;

	/* warning: no starvation prevention... */
	if (unlikely((skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL))
		return &q->internal;

	/* SYNACK messages are attached to a TCP_NEW_SYN_RECV request socket
	 * or a listener (SYNCOOKIE mode)
	 * 1) request sockets are not full blown,
	 *    they do not contain sk_pacing_rate
	 * 2) They are not part of a 'flow' yet
	 * 3) We do not want to rate limit them (eg SYNFLOOD attack),
	 *    especially if the listener set SO_MAX_PACING_RATE
	 * 4) We pretend they are orphaned
	 */
	if (!sk || sk_listener(sk)) {
		unsigned long hash = skb_get_hash(skb) & q->orphan_mask;

		/* By forcing low order bit to 1, we make sure to not
		 * collide with a local flow (socket pointers are word aligned)
		 */
		sk = (struct sock *)((hash << 1) | 1UL);
		skb_orphan(skb);
	}

	root = &q->cn_root[hash_ptr(sk, q->cn_trees_log)];

	if (q->flows >= (2U << q->cn_trees_log) &&
			q->inactive_flows > q->flows/2)
		cn_gc(q, root, sk);

	p = &root->rb_node;
	parent = NULL;
	while (*p) {
		parent = *p;

		f = rb_entry(parent, struct cn_flow, cn_node);
		if (f->sk == sk) {
			/* socket might have been reallocated, so check
			 * if its sk_hash is the same.
			 * It not, we need to refill credit with
			 * initial quantum
			 */
			if (unlikely(skb->sk &&
						 f->socket_hash != sk->sk_hash)) {
				f->credit = q->initial_quantum;
				f->socket_hash = sk->sk_hash;
				if (cn_flow_is_throttled(f))
					cn_flow_unset_throttled(q, f);
				f->time_next_packet = 0ULL;
			}
			return f;
		}
		if (f->sk > sk)
			p = &parent->rb_right;
		else
			p = &parent->rb_left;
	}

	f = kmem_cache_zalloc(cn_flow_cachep, GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!f)) {
		q->stat_allocation_errors++;
		return &q->internal;
	}
	cn_flow_set_detached(f);
	f->sk = sk;
	if (skb->sk)
		f->socket_hash = sk->sk_hash;
	f->credit = q->initial_quantum;

	rb_link_node(&f->cn_node, parent, p);
	rb_insert_color(&f->cn_node, root);

	q->flows++;
	q->inactive_flows++;
	ft = get_five_tuple_from_skb(skb);
	f->ft = ft;

	f->flow_start_ns = ktime_get_ns();
	if (ft.transport_protocol == 6) {
		struct ipv4_address src_ip = get_ip(ft.src_ip);
		struct ipv4_address dst_ip = get_ip(ft.dst_ip);
		trace_printk("sch_cn: At %llu, got new flow: src_ip=%s, dst_ip=%s, proto=%u, src_port=%u, dst_port=%u!\n", seconds_from_ns(f->flow_start_ns), src_ip.bytes, dst_ip.bytes, (u32) ft.transport_protocol, (u32) ft.src_port, (u32) ft.dst_port);
	}

	cn_initialize_monitoring_interval(f);
	cn_initialize_interval(f);
	f->longest_interval = f->current_interval;
	f->enlarge = false;
	f->flow_max_qlen = (u32) q->flow_plimit;
	return f;
}


/* remove one skb from head of flow queue */
static struct sk_buff *cn_dequeue_head(struct Qdisc *sch, struct cn_flow *flow)
{
	struct sk_buff *skb = flow->head;

	if (skb) {
		flow->head = skb->next;
		skb->next = NULL;
		flow->qlen--;
		flow->current_interval.packets_transmitted++;
		if (flow->qlen <= 0) {
			flow->idle = true;
			flow->became_idle_ns = ktime_get_ns();
		}
		qdisc_qstats_backlog_dec(sch, skb);
		flow->current_interval.min_queue_length = min(flow->current_interval.min_queue_length, (u64) flow->qlen);
		sch->q.qlen--;
	}
	return skb;
}

/* We might add in the future detection of retransmits
 * For the time being, just return false
 */
static bool skb_is_retransmit(struct sk_buff *skb)
{
	return false;
}

/* add skb to flow queue
 * flow queue is a linked list, kind of FIFO, except for TCP retransmits
 * We special case tcp retransmits to be transmitted before other packets.
 * We rely on fact that TCP retransmits are unlikely, so we do not waste
 * a separate queue or a pointer.
 * head->  [retrans pkt 1]
 *         [retrans pkt 2]
 *         [ normal pkt 1]
 *         [ normal pkt 2]
 *         [ normal pkt 3]
 * tail->  [ normal pkt 4]
 */
static void flow_queue_add(struct cn_flow *flow, struct sk_buff *skb)
{
	struct sk_buff *prev, *head = flow->head;

	skb->next = NULL;
	if (!head) {
		flow->head = skb;
		flow->tail = skb;
		return;
	}
	if (likely(!skb_is_retransmit(skb))) {
		flow->tail->next = skb;
		flow->tail = skb;
		return;
	}

	/* This skb is a tcp retransmit,
	 * find the last retrans packet in the queue
	 */
	prev = NULL;
	while (skb_is_retransmit(head)) {
		prev = head;
		head = head->next;
		if (!head)
			break;
	}
	if (!prev) { /* no rtx packet in queue, become the new head */
		skb->next = flow->head;
		flow->head = skb;
	} else {
		if (prev == flow->tail)
			flow->tail = skb;
		else
			skb->next = prev->next;
		prev->next = skb;
	}
}

static void cn_drop_packets_from_end(struct cn_flow *f, struct Qdisc *sch, struct sk_buff **to_free) {
	u64 number = f->longest_interval.min_queue_length;
	size_t i;
	u64 good_ones = f->qlen - number - 1;
	struct sk_buff *current_skb = f->head;
	struct sk_buff *superfluous_skb;
	struct sk_buff *next_superfluous_skb;
	size_t counter = 0;
	size_t drop_counter = 0;
	for (i=0; i < good_ones; i++) {
		current_skb = current_skb->next;
	}
	f->tail = current_skb;
	superfluous_skb = current_skb->next;
	while (superfluous_skb != NULL) {
		next_superfluous_skb = superfluous_skb->next;
		qdisc_drop(superfluous_skb, sch, to_free);
		superfluous_skb = next_superfluous_skb;
		drop_counter++;
	}
	current_skb->next = NULL;
	f->qlen -= number;
	sch->q.qlen -= number;
	f->flow_max_qlen -= number;
	if (f->ft.transport_protocol == 6) {
		trace_printk("sch_cn: 	At %llu, Dropped %llu packets, drop_counter=%lu!\n", seconds_from_ns(f->flow_start_ns), number, drop_counter);
	}
	if (f->flow_max_qlen <= 0) {
		trace_printk("sch_cn: At %llu, Oh no, f->flow_max_qlen=%d\n", seconds_from_ns(f->flow_start_ns), f->flow_max_qlen);
	}
	current_skb = f->head;
	while (current_skb != NULL) {
		counter++;
		current_skb = current_skb->next;
	}
	if (f->flow_max_qlen != counter) {
		trace_printk("sch_cn: At %llu, Oh no, f->flow_max_qlen=%d!=counter=%lu\n", seconds_from_ns(f->flow_start_ns), f->flow_max_qlen, counter);
}
}

static void cn_compute_and_set_new_monitoring_interval(struct cn_sched_data *q, struct cn_flow *f) {
	kernel_fpu_begin();
	f->monitoring_period_end_ns = (u64) (ktime_get_ns() + (u64) (((double) (f->longest_interval.end_ns - f->longest_interval.start_ns)) * (q->guard_interval)));
	kernel_fpu_end();
}

static int cn_enqueue(struct sk_buff *skb, struct Qdisc *sch,
					struct sk_buff **to_free)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct cn_flow *f;
	struct five_tuple ft;

	if (unlikely(sch->q.qlen >= sch->limit))
		return qdisc_drop(skb, sch, to_free);

	f = cn_classify(skb, q);
	if (unlikely(f->qlen >= f->flow_max_qlen && f != &q->internal)) {
		ft = f->ft;
		if (ft.transport_protocol == 6) {
			struct ipv4_address src_ip = get_ip(ft.src_ip);
			struct ipv4_address dst_ip = get_ip(ft.dst_ip);
			trace_printk("sch_cn: At %llu, Queue (%d, max %d packets) full for flow: src_ip=%s, dst_ip=%s, proto=%u, src_port=%u, dst_port=%u!\n", seconds_from_ns(f->flow_start_ns), f->qlen, f->flow_max_qlen, src_ip.bytes, dst_ip.bytes, (u32) ft.transport_protocol, (u32) ft.src_port, (u32) ft.dst_port);
		}
		// FIXME: This stat should probably be moved?
		q->stat_flows_plimit++;
		// Added by Max
		// Also, if the buffer was enlarged, just start a new monitoring interval.
		if (f->current_interval.idle_ns > 0 && !f->enlarge && f->monitoring_period_start_ns!=f->monitoring_period_end_ns) {
			u64 idle_interval;
			u64 active_interval;
			idle_interval = f->current_interval.idle_ns;
			active_interval = (ktime_get_ns() - f->current_interval.start_ns) - idle_interval;
			if (ft.transport_protocol == 6) {
				trace_printk("sch_cn: 	At %llu, Got idle ns: %llu, active ns: %llu, packets transmitted: %llu; enlarging buffer!\n", seconds_from_ns(f->flow_start_ns), idle_interval, active_interval, f->current_interval.packets_transmitted);
			}
			f->enlarge = true;
			kernel_fpu_begin();
			// TODO: This is not optimal as it doesn't consider other flows...
			f->flow_max_qlen = min(f->flow_max_qlen + ((int) (((double) f->current_interval.packets_transmitted)/active_interval*idle_interval)), (int) (((f->flow_max_qlen)*(q->max_increase))));
			kernel_fpu_end();
			if (ft.transport_protocol == 6) {
				trace_printk("sch_cn: 	At %llu, New queue length is %d!\n!", seconds_from_ns(f->flow_start_ns), f->flow_max_qlen);
			}
		} else {
			if (f->monitoring_period_start_ns==f->monitoring_period_end_ns || f->enlarge) {
				if (ft.transport_protocol == 6) {
					trace_printk("sch_cn: 	At %llu, Simply starting a new monitoring interval!\n", seconds_from_ns(f->flow_start_ns));
				}
				f->current_interval.end_ns = ktime_get_ns();
				f->longest_interval = f->current_interval;
				// cn_copy_longest_interval_if_needed(f);
				cn_initialize_monitoring_interval(f);
				cn_compute_and_set_new_monitoring_interval(q, f);
				cn_initialize_interval(f);
				f->longest_interval = f->current_interval;
				f->enlarge = false;
				return qdisc_drop(skb, sch, to_free);
			} else if (ktime_get_ns() > f->monitoring_period_end_ns) {
				f->current_interval.end_ns = ktime_get_ns();
				cn_copy_longest_interval_if_needed(f);
				if (f->longest_interval.min_queue_length > 0) {
					cn_drop_packets_from_end(f, sch, to_free);
				}
				if (ft.transport_protocol == 6) {
					trace_printk("sch_cn: 	At %llu, Monitoring period is over and no idle ns! New queue length is %d, max is %d!\n", seconds_from_ns(f->flow_start_ns),  f->qlen, f->flow_max_qlen);
				}
				cn_initialize_monitoring_interval(f);
				cn_compute_and_set_new_monitoring_interval(q, f);
				cn_initialize_interval(f);
				f->longest_interval = f->current_interval;
				return qdisc_drop(skb, sch, to_free);
			} else {
				if (ft.transport_protocol == 6) {
					trace_printk("sch_cn: 	At %llu, Packet lost during monitoring period... Business as usual!\n", seconds_from_ns(f->flow_start_ns));
				}
				f->current_interval.end_ns = ktime_get_ns();
				cn_copy_longest_interval_if_needed(f);
				cn_initialize_interval(f);
				return qdisc_drop(skb, sch, to_free);
			}
		}
	}

	f->qlen++;

	if (f->idle) {
		f->idle = false;
		f->current_interval.idle_ns += ktime_get_ns() - f->became_idle_ns;
	}

	if (skb_is_retransmit(skb))
		q->stat_tcp_retrans++;
	qdisc_qstats_backlog_inc(sch, skb);
	if (cn_flow_is_detached(f)) {
		struct sock *sk = skb->sk;

		cn_flow_add_tail(&q->new_flows, f);
		if (time_after(jiffies, f->age + q->flow_refill_delay))
			f->credit = max_t(u32, f->credit, q->quantum);
		if (sk && q->rate_enable) {
			if (unlikely(smp_load_acquire(&sk->sk_pacing_status) !=
						 SK_PACING_FQ))
				smp_store_release(&sk->sk_pacing_status,
							SK_PACING_FQ);
		}
		q->inactive_flows--;
		// Changed by Max. Apparently flows can be idle but not detached.
		// f->current_interval.idle_ns += jiffies - f->age;
	}

	/* Note: this overwrites f->age */
	flow_queue_add(f, skb);

	if (unlikely(f == &q->internal)) {
		q->stat_internal_packets++;
	}
	sch->q.qlen++;

	return NET_XMIT_SUCCESS;
}

static void cn_check_throttled(struct cn_sched_data *q, u64 now)
{
	unsigned long sample;
	struct rb_node *p;

	if (q->time_next_delayed_flow > now)
		return;

	/* Update unthrottle latency EWMA.
	 * This is cheap and can help diagnosing timer/latency problems.
	 */
	sample = (unsigned long)(now - q->time_next_delayed_flow);
	q->unthrottle_latency_ns -= q->unthrottle_latency_ns >> 3;
	q->unthrottle_latency_ns += sample >> 3;

	q->time_next_delayed_flow = ~0ULL;
	while ((p = rb_first(&q->delayed)) != NULL) {
		struct cn_flow *f = rb_entry(p, struct cn_flow, rate_node);

		if (f->time_next_packet > now) {
			q->time_next_delayed_flow = f->time_next_packet;
			break;
		}
		cn_flow_unset_throttled(q, f);
	}
}

static struct sk_buff *cn_dequeue(struct Qdisc *sch)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	u64 now = ktime_get_ns();
	struct cn_flow_head *head;
	struct sk_buff *skb;
	struct cn_flow *f;
	u32 rate, plen;

	skb = cn_dequeue_head(sch, &q->internal);
	if (skb)
		goto out;
	cn_check_throttled(q, now);
begin:
	head = &q->new_flows;
	if (!head->first) {
		head = &q->old_flows;
		if (!head->first) {
			if (q->time_next_delayed_flow != ~0ULL)
				qdisc_watchdog_schedule_ns(&q->watchdog,
								 q->time_next_delayed_flow);
			return NULL;
		}
	}
	f = head->first;

	if (f->credit <= 0) {
		f->credit += q->quantum;
		head->first = f->next;
		cn_flow_add_tail(&q->old_flows, f);
		goto begin;
	}

	skb = f->head;
	if (unlikely(skb && now < f->time_next_packet &&
				 !skb_is_tcp_pure_ack(skb))) {
		head->first = f->next;
		cn_flow_set_throttled(q, f);
		goto begin;
	}

	skb = cn_dequeue_head(sch, f);
	if (!skb) {
		head->first = f->next;
		/* force a pass through old_flows to prevent starvation */
		if ((head == &q->new_flows) && q->old_flows.first) {
			cn_flow_add_tail(&q->old_flows, f);
		} else {
			cn_flow_set_detached(f);
			q->inactive_flows++;
		}
		goto begin;
	}
	prefetch(&skb->end);
	f->credit -= qdisc_pkt_len(skb);

	if (!q->rate_enable)
		goto out;

	/* Do not pace locally generated ack packets */
	if (skb_is_tcp_pure_ack(skb))
		goto out;

	rate = q->flow_max_rate;
	if (skb->sk)
		rate = min(skb->sk->sk_pacing_rate, rate);

	if (rate <= q->low_rate_threshold) {
		f->credit = 0;
		plen = qdisc_pkt_len(skb);
	} else {
		plen = max(qdisc_pkt_len(skb), q->quantum);
		if (f->credit > 0)
			goto out;
	}
	if (rate != ~0U) {
		u64 len = (u64)plen * NSEC_PER_SEC;

		if (likely(rate))
			do_div(len, rate);
		/* Since socket rate can change later,
		 * clamp the delay to 1 second.
		 * Really, providers of too big packets should be fixed !
		 */
		if (unlikely(len > NSEC_PER_SEC)) {
			len = NSEC_PER_SEC;
			q->stat_pkts_too_long++;
		}
		/* Account for schedule/timers drifts.
		 * f->time_next_packet was set when prior packet was sent,
		 * and current time (@now) can be too late by tens of us.
		 */
		if (f->time_next_packet)
			len -= min(len/2, now - f->time_next_packet);
		f->time_next_packet = now + len;
	}
out:
	qdisc_bstats_update(sch, skb);
	return skb;
}

static void cn_flow_purge(struct cn_flow *flow)
{
	rtnl_kfree_skbs(flow->head, flow->tail);
	flow->head = NULL;
	flow->qlen = 0;
}

static void cn_reset(struct Qdisc *sch)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct rb_root *root;
	struct rb_node *p;
	struct cn_flow *f;
	unsigned int idx;

	sch->q.qlen = 0;
	sch->qstats.backlog = 0;

	cn_flow_purge(&q->internal);

	if (!q->cn_root)
		return;

	for (idx = 0; idx < (1U << q->cn_trees_log); idx++) {
		root = &q->cn_root[idx];
		while ((p = rb_first(root)) != NULL) {
			f = rb_entry(p, struct cn_flow, cn_node);
			rb_erase(p, root);

			cn_flow_purge(f);

			kmem_cache_free(cn_flow_cachep, f);
		}
	}
	q->new_flows.first	= NULL;
	q->old_flows.first	= NULL;
	q->delayed		= RB_ROOT;
	q->flows		= 0;
	q->inactive_flows	= 0;
	q->throttled_flows	= 0;
}

static void cn_rehash(struct cn_sched_data *q,
					struct rb_root *old_array, u32 old_log,
					struct rb_root *new_array, u32 new_log)
{
	struct rb_node *op, **np, *parent;
	struct rb_root *oroot, *nroot;
	struct cn_flow *of, *nf;
	int fcnt = 0;
	u32 idx;

	for (idx = 0; idx < (1U << old_log); idx++) {
		oroot = &old_array[idx];
		while ((op = rb_first(oroot)) != NULL) {
			rb_erase(op, oroot);
			of = rb_entry(op, struct cn_flow, cn_node);
			if (cn_gc_candidate(of)) {
				fcnt++;
				kmem_cache_free(cn_flow_cachep, of);
				continue;
			}
			nroot = &new_array[hash_ptr(of->sk, new_log)];

			np = &nroot->rb_node;
			parent = NULL;
			while (*np) {
				parent = *np;

				nf = rb_entry(parent, struct cn_flow, cn_node);
				BUG_ON(nf->sk == of->sk);

				if (nf->sk > of->sk)
					np = &parent->rb_right;
				else
					np = &parent->rb_left;
			}

			rb_link_node(&of->cn_node, parent, np);
			rb_insert_color(&of->cn_node, nroot);
		}
	}
	q->flows -= fcnt;
	q->inactive_flows -= fcnt;
	q->stat_gc_flows += fcnt;
}

static void cn_free(void *addr)
{
	kvfree(addr);
}

static int cn_resize(struct Qdisc *sch, u32 log)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct rb_root *array;
	void *old_cn_root;
	u32 idx;

	if (q->cn_root && log == q->cn_trees_log)
		return 0;

	/* If XPS was setup, we can allocate memory on right NUMA node */
	array = kvmalloc_node(sizeof(struct rb_root) << log, GFP_KERNEL | __GFP_RETRY_MAYFAIL,
						netdev_queue_numa_node_read(sch->dev_queue));
	if (!array)
		return -ENOMEM;

	for (idx = 0; idx < (1U << log); idx++)
		array[idx] = RB_ROOT;

	sch_tree_lock(sch);

	old_cn_root = q->cn_root;
	if (old_cn_root)
		cn_rehash(q, old_cn_root, q->cn_trees_log, array, log);

	q->cn_root = array;
	q->cn_trees_log = log;

	sch_tree_unlock(sch);

	cn_free(old_cn_root);

	return 0;
}

static const struct nla_policy cn_policy[TCA_CN_MAX + 1] = {
	[TCA_CN_PLIMIT]			= { .type = NLA_U32 },
	[TCA_CN_FLOW_PLIMIT]		= { .type = NLA_U32 },
	[TCA_CN_QUANTUM]		= { .type = NLA_U32 },
	[TCA_CN_INITIAL_QUANTUM]	= { .type = NLA_U32 },
	[TCA_CN_RATE_ENABLE]		= { .type = NLA_U32 },
	[TCA_CN_FLOW_DEFAULT_RATE]	= { .type = NLA_U32 },
	[TCA_CN_FLOW_MAX_RATE]		= { .type = NLA_U32 },
	[TCA_CN_BUCKETS_LOG]		= { .type = NLA_U32 },
	[TCA_CN_FLOW_REFILL_DELAY]	= { .type = NLA_U32 },
	[TCA_CN_LOW_RATE_THRESHOLD]	= { .type = NLA_U32 },
	// Of course floating point types are not supported 😒
	// [TCA_CN_GUARD_INTERVAL]	= { .type = NLA_UNSPEC },
	[TCA_CN_GUARD_INTERVAL]	= { .type = NLA_U64 },
};

static int cn_change(struct Qdisc *sch, struct nlattr *opt,
				 struct netlink_ext_ack *extack)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_CN_MAX + 1];
	int err, drop_count = 0;
	unsigned drop_len = 0;
	u32 cn_log;

	// trace_printk("sch_cn: In cn_change\n");
	if (!opt) {
		return -EINVAL;
		trace_printk("sch_cn: !opt :/\n");
	}

	err = nla_parse_nested(tb, TCA_CN_MAX, opt, cn_policy, NULL);
	if (err < 0) {
		trace_printk("sch_cn: Parsing failed, message: %d\n", err);
		return err;
	}

	sch_tree_lock(sch);

	cn_log = q->cn_trees_log;

	if (tb[TCA_CN_BUCKETS_LOG]) {
		u32 nval = nla_get_u32(tb[TCA_CN_BUCKETS_LOG]);

		if (nval >= 1 && nval <= ilog2(256*1024))
			cn_log = nval;
		else
			err = -EINVAL;
	}
	if (tb[TCA_CN_PLIMIT])

		sch->limit = nla_get_u32(tb[TCA_CN_PLIMIT]);

	if (tb[TCA_CN_FLOW_PLIMIT]) {
		q->flow_plimit = nla_get_u32(tb[TCA_CN_FLOW_PLIMIT]);
		trace_printk("sch_cn: Set flow_limit to %u\n", (unsigned int) q->flow_plimit);
	}
	if (tb[TCA_CN_GUARD_INTERVAL]) {
		u64 unsigned_integer;
		kernel_fpu_begin();
		unsigned_integer = nla_get_u64(tb[TCA_CN_GUARD_INTERVAL]);
		q->guard_interval = *((double*) &unsigned_integer);
		// u64 original = *((u64*) tb[TCA_CN_GUARD_INTERVAL]);
		// u64 what_is_printed = (u64) (q->guard_interval);
		// u64 inverse = (u64) 1/(q->guard_interval);
		// bool is_positive = q->guard_interval > 0;
		trace_printk("sch_cn: Set guard interval to something but can only print truncated int %llu\n", (u64) q->guard_interval);
		kernel_fpu_end();
	}
	if (tb[TCA_CN_MAX_INCREASE]) {
		u64 unsigned_integer;
		kernel_fpu_begin();
		unsigned_integer = nla_get_u64(tb[TCA_CN_MAX_INCREASE]);
		q->max_increase = *((double*) &unsigned_integer);
		// u64 original = *((u64*) tb[TCA_CN_GUARD_INTERVAL]);
		// u64 what_is_printed = (u64) (q->guard_interval);
		// u64 inverse = (u64) 1/(q->guard_interval);
		// bool is_positive = q->guard_interval > 0;
		trace_printk("sch_cn: Set max increase to something but can only print truncated int %llu\n", (u64) q->max_increase);
		kernel_fpu_end();
	}

	if (tb[TCA_CN_QUANTUM]) {
		u32 quantum = nla_get_u32(tb[TCA_CN_QUANTUM]);

		if (quantum > 0)
			q->quantum = quantum;
		else
			err = -EINVAL;
	}

	if (tb[TCA_CN_INITIAL_QUANTUM])
		q->initial_quantum = nla_get_u32(tb[TCA_CN_INITIAL_QUANTUM]);

	if (tb[TCA_CN_FLOW_DEFAULT_RATE])
		pr_warn_ratelimited("sch_cn: defrate %u ignored.\n",
						nla_get_u32(tb[TCA_CN_FLOW_DEFAULT_RATE]));

	if (tb[TCA_CN_FLOW_MAX_RATE])
		q->flow_max_rate = nla_get_u32(tb[TCA_CN_FLOW_MAX_RATE]);

	if (tb[TCA_CN_LOW_RATE_THRESHOLD])
		q->low_rate_threshold =
			nla_get_u32(tb[TCA_CN_LOW_RATE_THRESHOLD]);

	if (tb[TCA_CN_RATE_ENABLE]) {
		u32 enable = nla_get_u32(tb[TCA_CN_RATE_ENABLE]);

		if (enable <= 1)
			q->rate_enable = enable;
		else
			err = -EINVAL;
	}

	if (tb[TCA_CN_FLOW_REFILL_DELAY]) {
		u32 usecs_delay = nla_get_u32(tb[TCA_CN_FLOW_REFILL_DELAY]) ;

		q->flow_refill_delay = usecs_to_jiffies(usecs_delay);
	}

	if (tb[TCA_CN_ORPHAN_MASK])
		q->orphan_mask = nla_get_u32(tb[TCA_CN_ORPHAN_MASK]);

	if (!err) {
		sch_tree_unlock(sch);
		err = cn_resize(sch, cn_log);
		sch_tree_lock(sch);
	}
	while (sch->q.qlen > sch->limit) {
		struct sk_buff *skb = cn_dequeue(sch);

		if (!skb)
			break;
		drop_len += qdisc_pkt_len(skb);
		rtnl_kfree_skbs(skb, skb);
		drop_count++;
	}
	qdisc_tree_reduce_backlog(sch, drop_count, drop_len);

	// trace_printk("sch_cn: In cn_change at the end\n");

	sch_tree_unlock(sch);
	return err;
}

static void cn_destroy(struct Qdisc *sch)
{
	struct cn_sched_data *q = qdisc_priv(sch);

	cn_reset(sch);
	cn_free(q->cn_root);
	qdisc_watchdog_cancel(&q->watchdog);
}

static int cn_init(struct Qdisc *sch, struct nlattr *opt,
			 struct netlink_ext_ack *extack)
{

	struct cn_sched_data *q = qdisc_priv(sch);
	int err;

	// trace_printk("TCA_CN_MAX in sch_cn is %u\n", TCA_CN_MAX);

	sch->limit		= 10000;
	q->flow_plimit		= 100;

	trace_printk("sch_cn: Launching qdisc; MTU=%u!\n", psched_mtu(qdisc_dev(sch)));
	q->quantum		= 2 * psched_mtu(qdisc_dev(sch));
	q->initial_quantum	= 10 * psched_mtu(qdisc_dev(sch));

	q->flow_refill_delay	= msecs_to_jiffies(40);
	q->flow_max_rate	= ~0U;
	q->time_next_delayed_flow = ~0ULL;
	q->rate_enable		= 1;
	q->new_flows.first	= NULL;
	q->old_flows.first	= NULL;
	q->delayed		= RB_ROOT;
	q->cn_root		= NULL;
	q->cn_trees_log		= ilog2(1024);
	q->orphan_mask		= 1024 - 1;
	q->low_rate_threshold	= 550000 / 8;
	kernel_fpu_begin();
	q->guard_interval = 2.0;
	q->max_increase = 2.0;
	// u64 what_is_printed = (u64) (q->guard_interval);
	// u64 inverse = (u64) 1/(q->guard_interval);
	// bool is_positive = q->guard_interval > 0;
	// trace_printk("sch_cn: Set guard interval to something (at the beginning) but can only print %llu, its inverse %llu and it is > 0: %u\n", what_is_printed, inverse, (unsigned int) is_positive);
	kernel_fpu_end();
	qdisc_watchdog_init(&q->watchdog, sch);

	if (opt) {
		err = cn_change(sch, opt, extack);
	} else
		err = cn_resize(sch, q->cn_trees_log);

	return err;
}

static int cn_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start(skb, TCA_OPTIONS);
	if (opts == NULL)
		goto nla_put_failure;

	/* TCA_CN_FLOW_DEFAULT_RATE is not used anymore */

	if (nla_put_u32(skb, TCA_CN_PLIMIT, sch->limit) ||
			nla_put_u32(skb, TCA_CN_FLOW_PLIMIT, q->flow_plimit) ||
			nla_put_u32(skb, TCA_CN_QUANTUM, q->quantum) ||
			nla_put_u32(skb, TCA_CN_INITIAL_QUANTUM, q->initial_quantum) ||
			nla_put_u32(skb, TCA_CN_RATE_ENABLE, q->rate_enable) ||
			nla_put_u32(skb, TCA_CN_FLOW_MAX_RATE, q->flow_max_rate) ||
			nla_put_u32(skb, TCA_CN_FLOW_REFILL_DELAY,
			jiffies_to_usecs(q->flow_refill_delay)) ||
			nla_put_u32(skb, TCA_CN_ORPHAN_MASK, q->orphan_mask) ||
			nla_put_u32(skb, TCA_CN_LOW_RATE_THRESHOLD,
			q->low_rate_threshold) ||
			nla_put_u32(skb, TCA_CN_BUCKETS_LOG, q->cn_trees_log))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	return -1;
}

static int cn_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct cn_sched_data *q = qdisc_priv(sch);
	struct tc_fq_qd_stats st;

	sch_tree_lock(sch);

	st.gc_flows		  = q->stat_gc_flows;
	st.highprio_packets	  = q->stat_internal_packets;
	st.tcp_retrans		  = q->stat_tcp_retrans;
	st.throttled		  = q->stat_throttled;
	st.flows_plimit		  = q->stat_flows_plimit;
	st.pkts_too_long	  = q->stat_pkts_too_long;
	st.allocation_errors	  = q->stat_allocation_errors;
	st.time_next_delayed_flow = q->time_next_delayed_flow - ktime_get_ns();
	st.flows		  = q->flows;
	st.inactive_flows	  = q->inactive_flows;
	st.throttled_flows	  = q->throttled_flows;
	st.unthrottle_latency_ns  = min_t(unsigned long,
						q->unthrottle_latency_ns, ~0U);
	sch_tree_unlock(sch);

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc_ops cn_qdisc_ops __read_mostly = {
	.id		=	"cn",
	.priv_size	=	sizeof(struct cn_sched_data),

	.enqueue	=	cn_enqueue,
	.dequeue	=	cn_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	cn_init,
	.reset		=	cn_reset,
	.destroy	=	cn_destroy,
	.change		=	cn_change,
	.dump		=	cn_dump,
	.dump_stats	=	cn_dump_stats,
	.owner		=	THIS_MODULE,
};

static int __init cn_module_init(void)
{
	int ret;

	cn_flow_cachep = kmem_cache_create("cn_flow_cache",
						 sizeof(struct cn_flow),
						 0, 0, NULL);
	if (!cn_flow_cachep)
		return -ENOMEM;

	ret = register_qdisc(&cn_qdisc_ops);
	if (ret)
		kmem_cache_destroy(cn_flow_cachep);
	return ret;
}

static void __exit cn_module_exit(void)
{
	unregister_qdisc(&cn_qdisc_ops);
	kmem_cache_destroy(cn_flow_cachep);
}

module_init(cn_module_init)
module_exit(cn_module_exit)
MODULE_AUTHOR("Eric Dumazet");
MODULE_LICENSE("GPL");
