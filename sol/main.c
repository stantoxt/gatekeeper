/*
 * Gatekeeper - DoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>

#include <rte_sched.h>

#include "gatekeeper_gk.h"
#include "gatekeeper_launch.h"
#include "gatekeeper_sol.h"

int sol_logtype;

#define SOL_LOG(level, ...) \
	rte_log_ratelimit(RTE_LOG_ ## level, sol_logtype, "GATEKEEPER SOL: " __VA_ARGS__)

/*
 * Gatekeeper request priority queue implementation.
 *
 * To implement the request priority queue, we maintain a linked list of
 * packets listed in order of highest priority to lowest priority. We keep
 * an array where each index represents a priority, and each element of
 * that array holds a reference to the last packet of that priority.
 * This allows us to quickly insert new packets of any priority into
 * the linked list and drop the packet of lowest priority if necessary.
 */

static inline struct list_head *
mbuf_to_list(struct rte_mbuf *m)
{
	return rte_mbuf_to_priv(m);
}

/*
 * This function doesn't require that the list field of
 * a request to be initialized.
 */
static void
insert_new_priority_req(struct req_queue *req_queue, struct rte_mbuf *req,
	uint8_t priority)
{
	uint8_t next;

	/* This should be the first request of @priority. */
	RTE_VERIFY(req_queue->priorities[priority] == NULL);

	req_queue->priorities[priority] = req;

	/* This is the first packet in the queue. */
	if (req_queue->len == 0) {
		list_add(mbuf_to_list(req), &req_queue->head);
		req_queue->highest_priority = priority;
		req_queue->lowest_priority = priority;
		return;
	}

	/* Not the first packet, but still insert at the head of the queue. */
	if (priority > req_queue->highest_priority) {
		list_add(mbuf_to_list(req), &req_queue->head);
		req_queue->highest_priority = priority;
		return;
	}

	/*
	 * Insert in middle or end of queue.
	 */

	if (priority < req_queue->lowest_priority)
		req_queue->lowest_priority = priority;

	/*
	 * This function only inserts a request for a priority that
	 * does not currently exist, and there is at least one
	 * request of a higher priority already in the queue.
	 */
	RTE_VERIFY(priority != req_queue->highest_priority);
	for (next = priority + 1; next <= req_queue->highest_priority; next++) {
		if (req_queue->priorities[next] != NULL) {
			list_add(mbuf_to_list(req),
				mbuf_to_list(req_queue->priorities[next]));
			return;
		}
	}

	rte_panic("sol: %s failed to insert a request of a new priority\n",
		__func__);
}

/*
 * Get the rte_mbuf struct for this entry.
 * XXX #52 This function should be part of DPDK.
 */
static inline struct rte_mbuf *
rte_priv_to_mbuf(void *ptr)
{
	return RTE_PTR_SUB(ptr, sizeof(struct rte_mbuf));
}

static inline struct rte_mbuf *
list_to_mbuf(struct list_head *list)
{
	return rte_priv_to_mbuf(list);
}

/*
 * Get the first rte_mbuf element from a list.
 * Note, that list is expected to be not empty.
 */
static inline struct rte_mbuf *
list_first_entry_m(struct list_head *ptr)
{
	return list_to_mbuf(ptr->next);
}

/*
 * Get the last rte_mbuf element from a list.
 * Note, that list is expected to be not empty.
 */
static inline struct rte_mbuf *
list_last_entry_m(struct list_head *ptr)
{
	return list_to_mbuf(ptr->prev);
}

/* Get the prev rte_mbuf element in list. */
static inline struct rte_mbuf *
list_prev_entry_m(struct rte_mbuf *pos)
{
	return list_to_mbuf(mbuf_to_list(pos)->prev);
}

/* Get the next rte_mbuf element in list. */
static inline struct rte_mbuf *
list_next_entry_m(struct rte_mbuf *pos)
{
	return list_to_mbuf(mbuf_to_list(pos)->next);
}

static void
drop_lowest_priority_pkt(struct req_queue *req_queue)
{
	struct rte_mbuf *lowest_pr = list_last_entry_m(&req_queue->head);
	struct rte_mbuf *next_lowest_pr;

	RTE_VERIFY(req_queue->len > 0);

	if (unlikely(req_queue->len == 1)) {
		req_queue->priorities[lowest_pr->udata64] = NULL;
		req_queue->highest_priority = 0;
		req_queue->lowest_priority = GK_MAX_REQ_PRIORITY;
		goto drop;
	}

	next_lowest_pr = list_prev_entry_m(lowest_pr);

	/* The lowest priority packet was the only one of that priority. */
	if (lowest_pr->udata64 != next_lowest_pr->udata64) {
		req_queue->priorities[lowest_pr->udata64] = NULL;
		req_queue->lowest_priority = next_lowest_pr->udata64;
		goto drop;
	}

	req_queue->priorities[lowest_pr->udata64] = next_lowest_pr;

drop:
	list_del(mbuf_to_list(lowest_pr));
	rte_pktmbuf_free(lowest_pr);
	req_queue->len--;
}

/*
 * This function doesn't require that the list field of
 * a request to be initialized.
 */
static void
enqueue_req(struct sol_config *sol_conf, struct rte_mbuf *req)
{
	struct req_queue *req_queue = &sol_conf->req_queue;
	uint8_t priority = req->udata64;

	if (unlikely(priority > GK_MAX_REQ_PRIORITY)) {
		SOL_LOG(WARNING, "Trying to enqueue a request with priority %hhu, but should be in range [0, %d]. Overwrite the priority to PRIORITY_REQ_MIN (%hhu)\n",
			priority, GK_MAX_REQ_PRIORITY, PRIORITY_REQ_MIN);
		req->udata64 = PRIORITY_REQ_MIN;
		priority = PRIORITY_REQ_MIN;
	}

	if (req_queue->len >= sol_conf->pri_req_max_len) {
		/* New packet is lowest priority, so drop it. */
		if (req_queue->lowest_priority >= priority) {
			rte_pktmbuf_free(req);
			return;
		}
		drop_lowest_priority_pkt(req_queue);
	}

	if (req_queue->priorities[priority] == NULL) {
		/* Insert request of a priority we don't yet have. */
		insert_new_priority_req(req_queue, req, priority);
	} else {
		/* Append request to end of the appropriate priority. */
		list_add(mbuf_to_list(req), mbuf_to_list(
			req_queue->priorities[priority]));
		req_queue->priorities[priority] = req;
	}

	req_queue->len++;
}

static void
enqueue_reqs(struct sol_config *sol_conf)
{
	struct rte_mbuf *reqs[sol_conf->enq_burst_size];
	int num_reqs = rte_ring_sc_dequeue_burst(sol_conf->ring,
		(void **)reqs, sol_conf->enq_burst_size, NULL);
	int i;
	for (i = 0; i < num_reqs; i++)
		enqueue_req(sol_conf, reqs[i]);
}

static inline void
credits_update(struct req_queue *req_queue)
{
	uint64_t curr_cycles = rte_rdtsc();
	uint64_t avail_cycles = curr_cycles - req_queue->time_cpu_cycles;
#if __WORDSIZE == 64
	ldiv_t avail_bytes;
#elif __WORDSIZE == 32
	lldiv_t avail_bytes;
#else
	#error "unexpected value for __WORDSIZE macro"
#endif

	/* Not enough cycles have passed to update the number of credits. */
	if (avail_cycles <= req_queue->cycles_per_byte_floor)
		return;

#if __WORDSIZE == 64
	avail_bytes = ldiv(avail_cycles * req_queue->cycles_per_byte_b,
		req_queue->cycles_per_byte_a);
#elif __WORDSIZE == 32
	avail_bytes = lldiv(avail_cycles * req_queue->cycles_per_byte_b,
		req_queue->cycles_per_byte_a);
#else
	#error "unexpected value for __WORDSIZE macro"
#endif

	req_queue->tb_credit_bytes += avail_bytes.quot;
	if (req_queue->tb_credit_bytes > req_queue->tb_max_credit_bytes)
		req_queue->tb_credit_bytes = req_queue->tb_max_credit_bytes;

	/*
	 * If there are spare cycles (that were not converted to credits
	 * because of rounding), keep them for the next iteration.
	 */
	req_queue->time_cpu_cycles = curr_cycles -
		avail_bytes.rem / req_queue->cycles_per_byte_b;

}

static inline int
credits_check(struct req_queue *req_queue, struct rte_mbuf *pkt)
{
	/* Need to include Ethernet frame overhead (preamble, gap, etc.) */
	uint32_t pkt_len = pkt->pkt_len + RTE_SCHED_FRAME_OVERHEAD_DEFAULT;
	if (pkt_len > req_queue->tb_credit_bytes)
		return false;
	req_queue->tb_credit_bytes -= pkt_len;
	return true;
}

/*
 * Iterate over list of rte_mbufs safe against removal of list entry.
 */
#define list_for_each_entry_safe_m(pos, n, head)	\
	for (pos = list_first_entry_m(head),		\
			n = list_next_entry_m(pos);	\
		mbuf_to_list(pos) != (head);		\
		pos = n, n = list_next_entry_m(n))

static void
dequeue_reqs(struct sol_config *sol_conf, uint8_t tx_port)
{
	struct req_queue *req_queue = &sol_conf->req_queue;
	struct rte_mbuf *entry, *next;

	struct rte_mbuf *pkts_out[sol_conf->deq_burst_size];
	uint32_t nb_pkts_out = 0;
	uint16_t total_sent = 0;

	/* Get an up-to-date view of our credits. */
	credits_update(req_queue);

	list_for_each_entry_safe_m(entry, next, &req_queue->head) {
		if (!credits_check(req_queue, entry)) {
			/*
			 * The library log_ratelimit will throtle
			 * the log rate of the log entry below when
			 * Gatekeeper servers are under attacks.
			 */
			SOL_LOG(NOTICE, "Out of request bandwidth\n");
			goto out;
		}

		if (req_queue->len == 1 || (entry->udata64 != next->udata64))
			req_queue->priorities[entry->udata64] = NULL;
		list_del(mbuf_to_list(entry));
		req_queue->len--;

		pkts_out[nb_pkts_out++] = entry;

		if (nb_pkts_out >= sol_conf->deq_burst_size)
			break;
	}

out:
	if (list_empty(&req_queue->head)) {
		req_queue->highest_priority = 0;
		req_queue->lowest_priority = GK_MAX_REQ_PRIORITY;
	} else {
		struct rte_mbuf *first = list_first_entry_m(&req_queue->head);
		req_queue->highest_priority = first->udata64;
	}

	/* We cannot drop the packets, so re-send. */
	while (nb_pkts_out > 0) {
		uint16_t sent = rte_eth_tx_burst(tx_port,
			sol_conf->tx_queue_back,
			pkts_out + total_sent, nb_pkts_out);
		total_sent += sent;
		nb_pkts_out -= sent;
	}
}

static inline double
mbits_to_bytes(double mbps)
{
	return mbps * (1000 * 1000 / 8);
}

/*
 * Retrieve the link speed of a Gatekeeper interface. If it
 * is a bonded interface, the link speeds are summed.
 */
static int
iface_speed_bytes(struct gatekeeper_if *iface, uint64_t *link_speed_bytes)
{
	uint64_t link_speed_mbits = 0;
	uint8_t i;

	RTE_VERIFY(link_speed_bytes != NULL);

	for (i = 0; i < iface->num_ports; i++) {
		struct rte_eth_link link;
		rte_eth_link_get(iface->ports[i], &link);

		if (link.link_speed == ETH_SPEED_NUM_NONE) {
			*link_speed_bytes = 0;
			return -1;
		}

		link_speed_mbits += link.link_speed;
	}

	/* Convert to bytes per second. */
	*link_speed_bytes = mbits_to_bytes(link_speed_mbits);
	return 0;
}

/*
 * @sol_conf is allocated using rte_calloc_socket(), so initializations
 * to 0 are not strictly necessary in this function.
 */
static int
req_queue_init(struct sol_config *sol_conf)
{
	struct req_queue *req_queue = &sol_conf->req_queue;
	uint64_t link_speed_bytes;
	double max_credit_bytes_precise;
	double cycles_per_byte_precise;
	uint32_t a, b;
	int ret;

	req_queue->len = 0;
	req_queue->highest_priority = 0;
	req_queue->lowest_priority = GK_MAX_REQ_PRIORITY;

	/* Find link speed in bytes, even for a bonded interface. */
	ret = iface_speed_bytes(&sol_conf->net->back, &link_speed_bytes);
	if (ret == 0) {
		SOL_LOG(NOTICE,
			"Back interface link speed: %"PRIu64" bytes per second\n",
			link_speed_bytes);
		/* Keep max number of bytes a float for later calculations. */
		max_credit_bytes_precise =
			sol_conf->req_bw_rate * link_speed_bytes;
	} else {
		SOL_LOG(NOTICE, "Back interface link speed: undefined\n");
		if (sol_conf->req_channel_bw_mbps <= 0) {
			SOL_LOG(ERR, "When link speed on back interface is undefined, parameter req_channel_bw_mbps must be calculated and defined\n");
			return -1;
		}
		max_credit_bytes_precise =
			mbits_to_bytes(sol_conf->req_channel_bw_mbps);
	}


	/* Initialize token bucket as full. */
	req_queue->tb_max_credit_bytes = round(max_credit_bytes_precise);
	req_queue->tb_credit_bytes = req_queue->tb_max_credit_bytes;

	/*
	 * Compute the number of cycles needed to credit the request queue
	 * with bytes. Represent this ratio of cycles per byte using two
	 * numbers -- a numerator and denominator.
	 *
	 * The function rte_approx() can only approximate a floating-point
	 * number between (0, 1). Therefore, approximate only the fractional
	 * part of the cycles per byte using rte_approx(), and then add
	 * the integer number of cycles per byte to the numerator.
	 */
	cycles_per_byte_precise = cycles_per_sec / max_credit_bytes_precise;
	req_queue->cycles_per_byte_floor = cycles_per_byte_precise;
	ret = rte_approx(
		cycles_per_byte_precise - req_queue->cycles_per_byte_floor,
		sol_conf->tb_rate_approx_err, &a, &b);
	if (ret < 0) {
		SOL_LOG(ERR, "Could not approximate the request queue's allocated bandwidth\n");
		return ret;
	}
	req_queue->cycles_per_byte_a = a;
	req_queue->cycles_per_byte_b = b;

	/* Add integer number of cycles per byte to numerator. */
	req_queue->cycles_per_byte_a +=
		req_queue->cycles_per_byte_floor * req_queue->cycles_per_byte_b;

	SOL_LOG(NOTICE, "Cycles per byte (%f) represented as a rational: %"PRIu64" / %"PRIu64"\n",
		cycles_per_byte_precise,
		req_queue->cycles_per_byte_a, req_queue->cycles_per_byte_b);

	req_queue->time_cpu_cycles = rte_rdtsc();
	return 0;
}

static int
cleanup_sol(struct sol_config *sol_conf)
{
	struct req_queue *req_queue = &sol_conf->req_queue;
	struct rte_mbuf *entry, *next;

	list_for_each_entry_safe_m(entry, next, &req_queue->head) {
		list_del(mbuf_to_list(entry));
		rte_pktmbuf_free(entry);
		req_queue->len--;
	}

	if (req_queue->len > 0)
		SOL_LOG(NOTICE, "Bug: removing all requests from the priority queue on cleanup leaves the queue length at %"PRIu32"\n",
			req_queue->len);

	rte_ring_free(sol_conf->ring);
	rte_free(sol_conf);
	return 0;
}

static int
sol_proc(void *arg)
{
	struct sol_config *sol_conf = (struct sol_config *)arg;
	unsigned int lcore = sol_conf->lcore_id;
	uint8_t tx_port_back = sol_conf->net->back.id;

	SOL_LOG(NOTICE,
		"The Solicitor block is running at lcore = %u\n", lcore);

	while (likely(!exiting)) {
		enqueue_reqs(sol_conf);
		dequeue_reqs(sol_conf, tx_port_back);
	}

	SOL_LOG(NOTICE,
		"The Solicitor block at lcore = %u is exiting\n", lcore);

	return cleanup_sol(sol_conf);
}

static int
sol_stage1(void *arg)
{
	struct sol_config *sol_conf = arg;
	int ret = get_queue_id(&sol_conf->net->back, QUEUE_TYPE_TX,
		sol_conf->lcore_id, NULL);
	if (ret < 0) {
		SOL_LOG(ERR, "Cannot assign a TX queue for the back interface for lcore %u\n",
			sol_conf->lcore_id);
		goto cleanup;
	}
	sol_conf->tx_queue_back = ret;

	return 0;

cleanup:
	cleanup_sol(sol_conf);
	return -1;
}

static int
sol_stage2(void *arg)
{
	struct sol_config *sol_conf = arg;
	int ret = req_queue_init(sol_conf);
	if (ret < 0)
		goto cleanup;

	return 0;

cleanup:
	cleanup_sol(sol_conf);
	return ret;
}

int
run_sol(struct net_config *net_conf, struct sol_config *sol_conf)
{
	int ret;
	uint16_t front_inc;

	if (net_conf == NULL || sol_conf == NULL) {
		ret = -1;
		goto out;
	}

	sol_logtype = rte_log_register("gatekeeper.sol");
	if (sol_logtype < 0) {
		ret = -1;
		goto out;
	}
	ret = rte_log_set_level(sol_logtype, sol_conf->log_level);
	if (ret < 0) {
		ret = -1;
		goto out;
	}
	sol_conf->log_type = sol_logtype;

	log_ratelimit_state_init(sol_conf->lcore_id,
		sol_conf->log_ratelimit_interval_ms,
		sol_conf->log_ratelimit_burst);

	if (!net_conf->back_iface_enabled) {
		SOL_LOG(ERR, "Back interface is required\n");
		ret = -1;
		goto out;
	}

	if (sol_conf->pri_req_max_len == 0) {
		SOL_LOG(ERR,
			"Priority queue max len must be greater than 0\n");
		ret = -1;
		goto out;
	}

	if (sol_conf->enq_burst_size == 0 || sol_conf->deq_burst_size == 0) {
		SOL_LOG(ERR, "Priority queue enqueue and dequeue sizes must both be greater than 0\n");
		ret = -1;
		goto out;
	}

	if (sol_conf->deq_burst_size > sol_conf->pri_req_max_len ||
			sol_conf->enq_burst_size > sol_conf->pri_req_max_len) {
		SOL_LOG(ERR, "Request queue enqueue and dequeue sizes must be less than the max length of the request queue\n");
		ret = -1;
		goto out;
	}

	if (sol_conf->req_bw_rate <= 0 || sol_conf->req_bw_rate >= 1) {
		SOL_LOG(ERR,
			"Request queue bandwidth must be in range (0, 1), but it has been specified as %f\n",
			sol_conf->req_bw_rate);
		ret = -1;
		goto out;
	}

	if (sol_conf->req_channel_bw_mbps < 0) {
		SOL_LOG(ERR,
			"Request channel bandwidth in Mbps must be greater than 0 when the NIC doesn't supply guaranteed bandwidth, but is %f\n",
			sol_conf->req_channel_bw_mbps);
		ret = -1;
		goto out;
	}

	/*
	 * Need to account for the packets in the following scenarios:
	 *
	 * (1) sol_conf->pri_req_max_len packets may sit at the ring;
	 * (2) sol_conf->pri_req_max_len packet may sit at the actually queue;
	 * (3) enqueue_reqs() temporarily adds sol_conf->enq_burst_size
	 *     more packets;
	 * (4) sol_conf->deq_burst_size does not count because dequeue_reqs()
	 *     only reduces the number of packets, that is, it does not add.
	 *
	 * Although the packets are going to the back interface,
	 * they are allocated at the front interface.
	 */
	front_inc = 2 * sol_conf->pri_req_max_len + sol_conf->enq_burst_size;
	net_conf->front.total_pkt_burst += front_inc;

	sol_conf->ring = rte_ring_create("sol_reqs_ring",
		rte_align32pow2(sol_conf->pri_req_max_len),
		rte_lcore_to_socket_id(sol_conf->lcore_id), RING_F_SC_DEQ);
	if (sol_conf->ring == NULL) {
		G_LOG(ERR,
			"mailbox: can't create ring sol_reqs_ring at lcore %u\n",
			sol_conf->lcore_id);
		ret = -1;
		goto burst;
	}

	ret = net_launch_at_stage1(net_conf, 0, 0, 0, 1, sol_stage1, sol_conf);
	if (ret < 0)
		goto ring;

	ret = launch_at_stage2(sol_stage2, sol_conf);
	if (ret < 0)
		goto stage1;

	ret = launch_at_stage3("sol", sol_proc, sol_conf, sol_conf->lcore_id);
	if (ret < 0)
		goto stage2;

	sol_conf->net = net_conf;

	ret = 0;
	goto out;

stage2:
	pop_n_at_stage2(1);
stage1:
	pop_n_at_stage1(1);
ring:
	rte_ring_free(sol_conf->ring);
burst:
	net_conf->front.total_pkt_burst -= front_inc;
out:
	return ret;
}

/*
 * There should be only one sol_config instance.
 * Return an error if trying to allocate the second instance.
 *
 * Use rte_calloc_socket() to zero-out the instance and initialize the
 * request queue list to guarantee that cleanup_sol() won't fail
 * during initialization.
 */
struct sol_config *
alloc_sol_conf(unsigned int lcore)
{
	struct sol_config *sol_conf;
	static rte_atomic16_t num_sol_conf_alloc = RTE_ATOMIC16_INIT(0);
	if (rte_atomic16_test_and_set(&num_sol_conf_alloc) != 1) {
		SOL_LOG(ERR, "Trying to allocate the second instance of struct sol_config\n");
		return NULL;
	}
	sol_conf = rte_calloc_socket("sol_config", 1,
		sizeof(struct sol_config), 0, rte_lcore_to_socket_id(lcore));
	if (sol_conf == NULL) {
		rte_atomic16_clear(&num_sol_conf_alloc);
		SOL_LOG(ERR, "Failed to allocate the first instance of struct sol_config\n");
		return NULL;
	}
	sol_conf->lcore_id = lcore;
	INIT_LIST_HEAD(&sol_conf->req_queue.head);
	return sol_conf;
}

int
gk_solicitor_enqueue_bulk(struct sol_config *sol_conf,
	struct rte_mbuf **pkts, uint16_t num_pkts)
{
	unsigned int num_enqueued = rte_ring_mp_enqueue_bulk(sol_conf->ring,
		(void **)pkts, num_pkts, NULL);
	if (unlikely(num_enqueued < num_pkts)) {
		SOL_LOG(ERR, "Failed to enqueue a bulk of %hu requests - only %u requests are enqueued\n",
			num_pkts, num_enqueued);
	}

	return num_enqueued;
}
