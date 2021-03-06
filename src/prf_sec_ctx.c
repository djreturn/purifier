/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2015 Vladimir Medvedkin <medvedkinv@gmail.com>
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rte_malloc.h>
#include <rte_jhash.h>
#include <rte_random.h>
#include <rte_atomic.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>

#include "prf_stateful.h"
#include "prf_acl.h"
#include "prf_sec_ctx.h"
#include "prf_sec_ctx_api.h"
#include "main.h"

uint32_t prf_embrionic_threshold;
uint32_t prf_syn_proxy_secret[2];

uint8_t
prf_compress_opt(struct prf_tcpopts *options)
{
	uint8_t data;
	uint16_t mss = options->mss;

	for (data = PRF_ARRAY_SIZE(prf_msstab) - 1; data ; data--)
		if (mss >= prf_msstab[data])
			break;
	data += (options->wscale<<2);
	data += (options->sackok<<6);
	return data;
}

uint32_t
prf_synproxy_hash(uint32_t saddr, uint32_t daddr, uint16_t sport,
		uint16_t dport, uint32_t count, int c)
{
	uint32_t tmp[4];

	tmp[0] = saddr;
	tmp[1] = daddr;
	tmp[2] = ((uint32_t)sport << 16) | dport;
	tmp[3] = count;
	return rte_jhash(&tmp, 4, prf_syn_proxy_secret[c]);
}

uint32_t
prf_synproxy_cookie_get(uint32_t saddr, uint32_t daddr,
			uint16_t sport,	uint32_t dport, uint32_t sseq,
			uint32_t count, uint32_t data)
{
	return (prf_synproxy_hash(saddr, daddr, sport, dport, 0, 0) +
		sseq + (count << PRF_COOKIEBITS) +
		((prf_synproxy_hash(saddr, daddr, sport, dport, count, 1) + data)
		& PRF_COOKIEMASK));
}

int
prf_synproxy_cookie(uint32_t cookie, uint32_t saddr, uint32_t daddr,
		uint16_t sport, uint16_t dport, uint32_t sseq,
		uint32_t count, uint32_t maxdiff)
{
	uint32_t diff;

	cookie -= prf_synproxy_hash(saddr, daddr, sport, dport, 0, 0) + sseq;

	diff = (count - (cookie >> PRF_COOKIEBITS)) & ((uint32_t)-1 >> PRF_COOKIEBITS);
	if (diff >= maxdiff)
		return -EINVAL;

	return ((cookie - prf_synproxy_hash(saddr, daddr, sport, dport, count - diff, 1)) & PRF_COOKIEMASK);
}

int
prf_synproxy_cookie_check(struct ipv4_hdr *iph, struct tcp_hdr *th,
			uint32_t time_min, struct prf_tcpopts *options)
{
	uint32_t cookie = rte_be_to_cpu_32(th->recv_ack) - 1;
	uint32_t seq = rte_be_to_cpu_32(th->sent_seq) - 1;
	int data = prf_synproxy_cookie(cookie, iph->src_addr, iph->dst_addr,
					th->src_port, th->dst_port, seq, time_min, 2);
	if (data < 0)
		return 1;
	if (data < (1<<7)) {
		options->mss =  prf_msstab[data & (PRF_ARRAY_SIZE(prf_msstab) - 1)];
		options->wscale = (data>>2) & 0xf;
		options->sackok = data>>6 & 0x1;
		return 0;
	}
	return 1;
}



struct prf_src_track_hash *
prf_src_track_hash_init(unsigned lcore_id, int idx)
{
	struct prf_src_track_hash *hash = NULL;
	char buf[PRF_TCP_HASH_NAMESIZE];

	snprintf(buf, sizeof(buf), "prf_src_track_hash_%u_%u", lcore_id, idx);
	hash = (struct prf_src_track_hash *)rte_zmalloc_socket(buf, sizeof(struct prf_src_track_hash), RTE_CACHE_LINE_SIZE, 0);
	return hash;
}

int
prf_src_track_node_add(struct prf_src_track_hash *hash_table,
		uint32_t key, struct prf_src_track_node **node)
{
	int ret, i = 0;
	uint32_t bucket;
	struct prf_src_track_ent *ent, *cur;

	bucket = rte_jhash_1word(key, prf_hash_initval) & PRF_SRC_TRACK_HASH_MASK;
	for (i = 0; i < PRF_SRC_TRACK_PRF_KEYS_PER_BUCKET; i++) {
		if (hash_table->key_bucket[bucket].key[i] == 0) {
			hash_table->key_bucket[bucket].key[i] = key;
			*node = &hash_table->node_bucket[bucket].node[i];
			(*node)->key = key;
			return 0;
		}
	}

	ret = rte_mempool_get(prf_src_track_pool, (void *)&ent);
	if (ret != 0)
		return -ENOENT;

	ent->next = NULL;
	ent->node.key = key;
	*node = &ent->node;

	if (hash_table->key_bucket[bucket].head == NULL) {
		hash_table->key_bucket[bucket].head = ent;
		return 0;
	}

	cur = hash_table->key_bucket[bucket].head;
	while (cur->next != NULL)
		cur = cur->next;

	cur->next = ent;
	return 0;
}

int
prf_src_track_node_del(struct prf_src_track_hash *hash_table, uint32_t key)
{
	int i;
	uint32_t bucket;
	struct prf_src_track_ent *tmp, *cur, **head;

	if (key == 0)
		return -EINVAL;

	bucket = rte_jhash_1word(key, prf_hash_initval) & PRF_SRC_TRACK_HASH_MASK;

	for (i = 0; i < PRF_SRC_TRACK_PRF_KEYS_PER_BUCKET; i++) {
		if (hash_table->key_bucket[bucket].key[i] == key) {
			hash_table->key_bucket[bucket].key[i] = 0;
			memset(&hash_table->node_bucket[bucket].node[i], 0, sizeof(struct prf_src_track_node));
			return 0;
		}
	}

	head = &(hash_table->key_bucket[bucket].head);
	if (*head == NULL)
		return -ENOENT;

	if ((*head)->node.key == key) {
		tmp = *head;
		*head = (*head)->next;
		memset(tmp, 0, sizeof(struct prf_src_track_node));
		rte_mempool_put(prf_src_track_pool, tmp);
		return 0;
	}

	cur = *head;
	while (cur->next != NULL) {
		if (cur->next->node.key == key) {
			tmp = cur->next;
			cur->next = tmp->next;
			memset(tmp, 0, sizeof(struct prf_src_track_node));
			rte_mempool_put(prf_src_track_pool, tmp);
			return 0;
		}
		cur = cur->next;
	}
	return -ENOENT;
}

int
prf_src_track_node_lookup(struct prf_src_track_hash *hash_table,
			uint32_t key, struct prf_src_track_node **node)
{
	int i;
	uint32_t bucket;
	struct prf_src_track_ent *cur;

	bucket = rte_jhash_1word(key, prf_hash_initval) & PRF_SRC_TRACK_HASH_MASK;
	for (i = 0; i < PRF_SRC_TRACK_PRF_KEYS_PER_BUCKET; i++) {
		if (hash_table->key_bucket[bucket].key[i] == key) {
			*node = &hash_table->node_bucket[bucket].node[i];
			return 0;
		}
	}

	cur = hash_table->key_bucket[bucket].head;
	while (cur) {
		if (cur->node.key == key) {
			*node = &cur->node;
			return 0;
		}
		cur = cur->next;
	}
	return -ENOENT;
}

int
prf_src_track_rate_check(struct prf_src_track_node *node,
			struct prf_sec_ctx_rule *rule, uint64_t time)
{
	uint64_t time_diff, n_periods;

	time_diff = time - node->time;
	n_periods = time_diff / rule->period;
	node->time += n_periods * rule->period;

	node->bucket += n_periods;
	if (node->bucket > rule->bucket_size)
		node->bucket = rule->bucket_size;

	if (node->bucket == 0)
		return 1;

	--node->bucket;
	return 0;
}

int
prf_src_track_checkout(struct prf_sec_ctx_rule *rule, uint32_t key,
			uint64_t time, struct prf_src_track_node **node)
{
	int ret;

	if (unlikely((rule == NULL) || (key == 0) || (time == 0)))
		return -EINVAL;

	if (likely((rule->flags & PRF_WHITE_LIST_CHECK) == PRF_WHITE_LIST_CHECK)) {
		ret = prf_ipset_lookup(rule->white_list, key, time);
		if (unlikely(ret == 0))
			return 0;
	}
	if (likely((rule->flags & PRF_BLACK_LIST_CHECK) == PRF_BLACK_LIST_CHECK)) {
		ret = prf_ipset_lookup(rule->black_list, key, time);
		if (unlikely(ret == 0))
			return 3;
	}

	ret = prf_src_track_node_lookup(rule->hash_table, key, node);

	if (ret == -ENOENT) {
		ret = prf_src_track_node_add(rule->hash_table, key, node);
		if (ret < 0)
			return ret;

		rte_atomic64_inc(&rule->ref_cnt);
		(*node)->rule		= rule;
		(*node)->counter	= 0;
		(*node)->time		= time;
		(*node)->bucket		= rule->bucket_size;
	}
	if ((rule->flags & PRF_SRC_TRACK_CONN_FLAG) == PRF_SRC_TRACK_CONN_FLAG) {
		if ((*node)->counter >= rule->max_states) {
			if ((rule->flags & PRF_SRC_TRACK_BAN) == PRF_SRC_TRACK_BAN) {
				ret = prf_ipset_add(rule->black_list, key, time);
/* TODO: ipset overflow statistics or LOG if ret = -ENOENT */
			}
			return 1;
		}
	}
	if ((rule->flags & PRF_SRC_TRACK_RATE_FLAG) == PRF_SRC_TRACK_RATE_FLAG) {
		ret = prf_src_track_rate_check(*node, rule, time);
		if (ret) {
			if ((rule->flags & PRF_SRC_TRACK_BAN) == PRF_SRC_TRACK_BAN) {
				ret = prf_ipset_add(rule->black_list, key, time);
			}
			return 2;
		}
	}
	++(*node)->counter;
	return 0;
}

struct prf_ipset_hash *
prf_ipset_hash_init(unsigned lcore_id, int idx)
{
	struct prf_ipset_hash *hash = NULL;
	char buf[PRF_TCP_HASH_NAMESIZE];

	snprintf(buf, sizeof(buf), "prf_ipset_hash_%u_%u", lcore_id, idx);
	hash = (struct prf_ipset_hash *)rte_zmalloc_socket(buf, sizeof(struct prf_ipset_hash), RTE_CACHE_LINE_SIZE, 0);
	return hash;
}


int
prf_ipset_lookup(struct prf_ipset_hash *hash, uint32_t key, uint64_t time)
{
	int i;
	uint32_t bucket;

	bucket = rte_jhash_1word(key, prf_hash_initval) & PRF_IPSET_HASH_MASK;

	rte_prefetch0((void *)&hash->bucket[bucket].key[0]);
	rte_prefetch0((void *)&hash->bucket[bucket].timer[0]);
	rte_prefetch0((void *)((char *)&hash->bucket[bucket].timer[0] + RTE_CACHE_LINE_SIZE));

	for (i = 0; i < PRF_NB_IPSET_KEYS; i++) {
		if (unlikely(hash->bucket[bucket].key[i] == key)) {
			if (unlikely((time - hash->bucket[bucket].timer[i]) > hash->ban_timer)) {
				hash->bucket[bucket].key[i]	= 0;
				hash->bucket[bucket].timer[i]	= 0;
				return -ENOENT;
			}
			if (hash->flags && PRF_IPSET_UPDATE_TIMER)
				hash->bucket[bucket].timer[i] = time;
			return 0;
		}
	}
	return -ENOENT;
}

int
prf_ipset_add(struct prf_ipset_hash *hash, uint32_t key, uint64_t time)
{
	int i;
	uint32_t bucket;

	bucket = rte_jhash_1word(key, prf_hash_initval) & PRF_IPSET_HASH_MASK;

	rte_prefetch0((void *)&hash->bucket[bucket].key[0]);
	rte_prefetch0((void *)&hash->bucket[bucket].timer[0]);
	rte_prefetch0((void *)((char *)&hash->bucket[bucket].timer[0] + RTE_CACHE_LINE_SIZE));

	for (i = 0; i < PRF_NB_IPSET_KEYS; i++) {
		if (unlikely(hash->bucket[bucket].key[i] == key)) {
			hash->bucket[bucket].timer[i]	= time;
			return 0;
		}
	}
	for (i = 0; i < PRF_NB_IPSET_KEYS; i++) {
		if (unlikely(time - hash->bucket[bucket].timer[i] > hash->ban_timer)) {
			hash->bucket[bucket].key[i]	= key;
			hash->bucket[bucket].timer[i]	= time;
			return 0;
		}
	}
	return -ENOENT;
}
