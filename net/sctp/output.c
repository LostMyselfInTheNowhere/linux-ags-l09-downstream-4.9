/* SCTP kernel implementation
 * (C) Copyright IBM Corp. 2001, 2004
 * Copyright (c) 1999-2000 Cisco, Inc.
 * Copyright (c) 1999-2001 Motorola, Inc.
 *
 * This file is part of the SCTP kernel implementation
 *
 * These functions handle output processing.
 *
 * This SCTP implementation is free software;
 * you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This SCTP implementation is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 ************************
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Please send any bug reports or fixes you make to the
 * email address(es):
 *    lksctp developers <linux-sctp@vger.kernel.org>
 *
 * Written or modified by:
 *    La Monte H.P. Yarroll <piggy@acm.org>
 *    Karl Knutson          <karl@athena.chicago.il.us>
 *    Jon Grimm             <jgrimm@austin.ibm.com>
 *    Sridhar Samudrala     <sri@us.ibm.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <net/inet_ecn.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/net_namespace.h>

#include <linux/socket.h> /* for sa_family_t */
#include <net/sock.h>

#include <net/sctp/sctp.h>
#include <net/sctp/sm.h>
#include <net/sctp/checksum.h>

/* Forward declarations for private helpers. */
static sctp_xmit_t __sctp_packet_append_chunk(struct sctp_packet *packet,
					      struct sctp_chunk *chunk);
static sctp_xmit_t sctp_packet_can_append_data(struct sctp_packet *packet,
					   struct sctp_chunk *chunk);
static void sctp_packet_append_data(struct sctp_packet *packet,
					   struct sctp_chunk *chunk);
static sctp_xmit_t sctp_packet_will_fit(struct sctp_packet *packet,
					struct sctp_chunk *chunk,
					u16 chunk_len);

static void sctp_packet_reset(struct sctp_packet *packet)
{
	packet->size = packet->overhead;
	packet->has_cookie_echo = 0;
	packet->has_sack = 0;
	packet->has_data = 0;
	packet->has_auth = 0;
	packet->ipfragok = 0;
	packet->auth = NULL;
}

/* Config a packet.
 * This appears to be a followup set of initializations.
 */
struct sctp_packet *sctp_packet_config(struct sctp_packet *packet,
				       __u32 vtag, int ecn_capable)
{
	struct sctp_transport *tp = packet->transport;
	struct sctp_association *asoc = tp->asoc;

	pr_debug("%s: packet:%p vtag:0x%x\n", __func__, packet, vtag);

	packet->vtag = vtag;

	if (asoc && tp->dst) {
		struct sock *sk = asoc->base.sk;

		rcu_read_lock();
		if (__sk_dst_get(sk) != tp->dst) {
			dst_hold(tp->dst);
			sk_setup_caps(sk, tp->dst);
		}

		if (sk_can_gso(sk)) {
			struct net_device *dev = tp->dst->dev;

			packet->max_size = dev->gso_max_size;
		} else {
			packet->max_size = asoc->pathmtu;
		}
		rcu_read_unlock();

	} else {
		packet->max_size = tp->pathmtu;
	}

	if (ecn_capable && sctp_packet_empty(packet)) {
		struct sctp_chunk *chunk;

		/* If there a is a prepend chunk stick it on the list before
		 * any other chunks get appended.
		 */
		chunk = sctp_get_ecne_prepend(asoc);
		if (chunk)
			sctp_packet_append_chunk(packet, chunk);
	}

	return packet;
}

/* Initialize the packet structure. */
struct sctp_packet *sctp_packet_init(struct sctp_packet *packet,
				     struct sctp_transport *transport,
				     __u16 sport, __u16 dport)
{
	struct sctp_association *asoc = transport->asoc;
	size_t overhead;

	pr_debug("%s: packet:%p transport:%p\n", __func__, packet, transport);

	packet->transport = transport;
	packet->source_port = sport;
	packet->destination_port = dport;
	INIT_LIST_HEAD(&packet->chunk_list);
	if (asoc) {
		struct sctp_sock *sp = sctp_sk(asoc->base.sk);
		overhead = sp->pf->af->net_header_len;
	} else {
		overhead = sizeof(struct ipv6hdr);
	}
	overhead += sizeof(struct sctphdr);
	packet->overhead = overhead;
	sctp_packet_reset(packet);
	packet->vtag = 0;

	return packet;
}

/* Free a packet.  */
void sctp_packet_free(struct sctp_packet *packet)
{
	struct sctp_chunk *chunk, *tmp;

	pr_debug("%s: packet:%p\n", __func__, packet);

	list_for_each_entry_safe(chunk, tmp, &packet->chunk_list, list) {
		list_del_init(&chunk->list);
		sctp_chunk_free(chunk);
	}
}

/* This routine tries to append the chunk to the offered packet. If adding
 * the chunk causes the packet to exceed the path MTU and COOKIE_ECHO chunk
 * is not present in the packet, it transmits the input packet.
 * Data can be bundled with a packet containing a COOKIE_ECHO chunk as long
 * as it can fit in the packet, but any more data that does not fit in this
 * packet can be sent only after receiving the COOKIE_ACK.
 */
sctp_xmit_t sctp_packet_transmit_chunk(struct sctp_packet *packet,
				       struct sctp_chunk *chunk,
				       int one_packet, gfp_t gfp)
{
	sctp_xmit_t retval;

	pr_debug("%s: packet:%p size:%Zu chunk:%p size:%d\n", __func__,
		 packet, packet->size, chunk, chunk->skb ? chunk->skb->len : -1);

	switch ((retval = (sctp_packet_append_chunk(packet, chunk)))) {
	case SCTP_XMIT_PMTU_FULL:
		if (!packet->has_cookie_echo) {
			int error = 0;

			error = sctp_packet_transmit(packet, gfp);
			if (error < 0)
				chunk->skb->sk->sk_err = -error;

			/* If we have an empty packet, then we can NOT ever
			 * return PMTU_FULL.
			 */
			if (!one_packet)
				retval = sctp_packet_append_chunk(packet,
								  chunk);
		}
		break;

	case SCTP_XMIT_RWND_FULL:
	case SCTP_XMIT_OK:
	case SCTP_XMIT_DELAY:
		break;
	}

	return retval;
}

/* Try to bundle an auth chunk into the packet. */
static sctp_xmit_t sctp_packet_bundle_auth(struct sctp_packet *pkt,
					   struct sctp_chunk *chunk)
{
	struct sctp_association *asoc = pkt->transport->asoc;
	struct sctp_chunk *auth;
	sctp_xmit_t retval = SCTP_XMIT_OK;

	/* if we don't have an association, we can't do authentication */
	if (!asoc)
		return retval;

	/* See if this is an auth chunk we are bundling or if
	 * auth is already bundled.
	 */
	if (chunk->chunk_hdr->type == SCTP_CID_AUTH || pkt->has_auth)
		return retval;

	/* if the peer did not request this chunk to be authenticated,
	 * don't do it
	 */
	if (!chunk->auth)
		return retval;

	auth = sctp_make_auth(asoc);
	if (!auth)
		return retval;

	retval = __sctp_packet_append_chunk(pkt, auth);

	if (retval != SCTP_XMIT_OK)
		sctp_chunk_free(auth);

	return retval;
}

/* Try to bundle a SACK with the packet. */
static sctp_xmit_t sctp_packet_bundle_sack(struct sctp_packet *pkt,
					   struct sctp_chunk *chunk)
{
	sctp_xmit_t retval = SCTP_XMIT_OK;

	/* If sending DATA and haven't aleady bundled a SACK, try to
	 * bundle one in to the packet.
	 */
	if (sctp_chunk_is_data(chunk) && !pkt->has_sack &&
	    !pkt->has_cookie_echo) {
		struct sctp_association *asoc;
		struct timer_list *timer;
		asoc = pkt->transport->asoc;
		timer = &asoc->timers[SCTP_EVENT_TIMEOUT_SACK];

		/* If the SACK timer is running, we have a pending SACK */
		if (timer_pending(timer)) {
			struct sctp_chunk *sack;

			if (pkt->transport->sack_generation !=
			    pkt->transport->asoc->peer.sack_generation)
				return retval;

			asoc->a_rwnd = asoc->rwnd;
			sack = sctp_make_sack(asoc);
			if (sack) {
				retval = __sctp_packet_append_chunk(pkt, sack);
				if (retval != SCTP_XMIT_OK) {
					sctp_chunk_free(sack);
					goto out;
				}
				asoc->peer.sack_needed = 0;
				if (del_timer(timer))
					sctp_association_put(asoc);
			}
		}
	}
out:
	return retval;
}


/* Append a chunk to the offered packet reporting back any inability to do
 * so.
 */
static sctp_xmit_t __sctp_packet_append_chunk(struct sctp_packet *packet,
					      struct sctp_chunk *chunk)
{
	sctp_xmit_t retval = SCTP_XMIT_OK;
	__u16 chunk_len = SCTP_PAD4(ntohs(chunk->chunk_hdr->length));

	/* Check to see if this chunk will fit into the packet */
	retval = sctp_packet_will_fit(packet, chunk, chunk_len);
	if (retval != SCTP_XMIT_OK)
		goto finish;

	/* We believe that this chunk is OK to add to the packet */
	switch (chunk->chunk_hdr->type) {
	case SCTP_CID_DATA:
		/* Account for the data being in the packet */
		sctp_packet_append_data(packet, chunk);
		/* Disallow SACK bundling after DATA. */
		packet->has_sack = 1;
		/* Disallow AUTH bundling after DATA */
		packet->has_auth = 1;
		/* Let it be knows that packet has DATA in it */
		packet->has_data = 1;
		/* timestamp the chunk for rtx purposes */
		chunk->sent_at = jiffies;
		/* Mainly used for prsctp RTX policy */
		chunk->sent_count++;
		break;
	case SCTP_CID_COOKIE_ECHO:
		packet->has_cookie_echo = 1;
		break;

	case SCTP_CID_SACK:
		packet->has_sack = 1;
		if (chunk->asoc)
			chunk->asoc->stats.osacks++;
		break;

	case SCTP_CID_AUTH:
		packet->has_auth = 1;
		packet->auth = chunk;
		break;
	}

	/* It is OK to send this chunk.  */
	list_add_tail(&chunk->list, &packet->chunk_list);
	packet->size += chunk_len;
	chunk->transport = packet->transport;
finish:
	return retval;
}

/* Append a chunk to the offered packet reporting back any inability to do
 * so.
 */
sctp_xmit_t sctp_packet_append_chunk(struct sctp_packet *packet,
				     struct sctp_chunk *chunk)
{
	sctp_xmit_t retval = SCTP_XMIT_OK;

	pr_debug("%s: packet:%p chunk:%p\n", __func__, packet, chunk);

	/* Data chunks are special.  Before seeing what else we can
	 * bundle into this packet, check to see if we are allowed to
	 * send this DATA.
	 */
	if (sctp_chunk_is_data(chunk)) {
		retval = sctp_packet_can_append_data(packet, chunk);
		if (retval != SCTP_XMIT_OK)
			goto finish;
	}

	/* Try to bundle AUTH chunk */
	retval = sctp_packet_bundle_auth(packet, chunk);
	if (retval != SCTP_XMIT_OK)
		goto finish;

	/* Try to bundle SACK chunk */
	retval = sctp_packet_bundle_sack(packet, chunk);
	if (retval != SCTP_XMIT_OK)
		goto finish;

	retval = __sctp_packet_append_chunk(packet, chunk);

finish:
	return retval;
}

static void sctp_packet_release_owner(struct sk_buff *skb)
{
	sk_free(skb->sk);
}

static void sctp_packet_set_owner_w(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);
	skb->sk = sk;
	skb->destructor = sctp_packet_release_owner;

	/*
	 * The data chunks have already been accounted for in sctp_sendmsg(),
	 * therefore only reserve a single byte to keep socket around until
	 * the packet has been transmitted.
	 */
	atomic_inc(&sk->sk_wmem_alloc);
}

/* All packets are sent to the network through this function from
 * sctp_outq_tail().
 *
 * The return value is a normal kernel error return value.
 */
int sctp_packet_transmit(struct sctp_packet *packet, gfp_t gfp)
{
	struct sctp_transport *tp = packet->transport;
	struct sctp_association *asoc = tp->asoc;
	struct sctphdr *sh;
	struct sk_buff *nskb = NULL, *head = NULL;
	struct sctp_chunk *chunk, *tmp;
	struct sock *sk;
	int err = 0;
	int padding;		/* How much padding do we need?  */
	int pkt_size;
	__u8 has_data = 0;
	int gso = 0;
	int pktcount = 0;
	int auth_len = 0;
	struct dst_entry *dst;
	unsigned char *auth = NULL;	/* pointer to auth in skb data */

	pr_debug("%s: packet:%p\n", __func__, packet);

	/* Do NOT generate a chunkless packet. */
	if (list_empty(&packet->chunk_list))
		return err;

	/* Set up convenience variables... */
	chunk = list_entry(packet->chunk_list.next, struct sctp_chunk, list);
	sk = chunk->skb->sk;

	/* Allocate the head skb, or main one if not in GSO */
	if (packet->size > tp->pathmtu && !packet->ipfragok) {
		if (sk_can_gso(sk)) {
			gso = 1;
			pkt_size = packet->overhead;
		} else {
			/* If this happens, we trash this packet and try
			 * to build a new one, hopefully correct this
			 * time. Application may notice this error.
			 */
			pr_err_once("Trying to GSO but underlying device doesn't support it.");
			goto err;
		}
	} else {
		pkt_size = packet->size;
	}
	head = alloc_skb(pkt_size + MAX_HEADER, gfp);
	if (!head)
		goto err;
	if (gso) {
		NAPI_GRO_CB(head)->last = head;
		skb_shinfo(head)->gso_type = sk->sk_gso_type;
	}

	/* Make sure the outbound skb has enough header room reserved. */
	skb_reserve(head, packet->overhead + MAX_HEADER);

	/* Set the owning socket so that we know where to get the
	 * destination IP address.
	 */
	sctp_packet_set_owner_w(head, sk);

	if (!sctp_transport_dst_check(tp)) {
		sctp_transport_route(tp, NULL, sctp_sk(sk));
		if (asoc && (asoc->param_flags & SPP_PMTUD_ENABLE)) {
			sctp_assoc_sync_pmtu(sk, asoc);
		}
	}
	dst = dst_clone(tp->dst);
	if (!dst) {
		if (asoc)
			IP_INC_STATS(sock_net(asoc->base.sk),
				     IPSTATS_MIB_OUTNOROUTES);
		goto nodst;
	}
	skb_dst_set(head, dst);

	/* Build the SCTP header.  */
	sh = (struct sctphdr *)skb_push(head, sizeof(struct sctphdr));
	skb_reset_transport_header(head);
	sh->source = htons(packet->source_port);
	sh->dest   = htons(packet->destination_port);

	/* From 6.8 Adler-32 Checksum Calculation:
	 * After the packet is constructed (containing the SCTP common
	 * header and one or more control or DATA chunks), the
	 * transmitter shall:
	 *
	 * 1) Fill in the proper Verification Tag in the SCTP common
	 *    header and initialize the checksum field to 0's.
	 */
	sh->vtag     = htonl(packet->vtag);
	sh->checksum = 0;

	pr_debug("***sctp_transmit_packet***\n");

	do {
		/* Set up convenience variables... */
		chunk = list_entry(packet->chunk_list.next, struct sctp_chunk, list);
		pktcount++;

		/* Calculate packet size, so it fits in PMTU. Leave
		 * other chunks for the next packets.
		 */
		if (gso) {
			pkt_size = packet->overhead;
			list_for_each_entry(chunk, &packet->chunk_list, list) {
				int padded = SCTP_PAD4(chunk->skb->len);

				if (chunk == packet->auth)
					auth_len = padded;
				else if (auth_len + padded + packet->overhead >
					 tp->pathmtu)
					goto nomem;
				else if (pkt_size + padded > tp->pathmtu)
					break;
				pkt_size += padded;
			}

			/* Allocate a new skb. */
			nskb = alloc_skb(pkt_size + MAX_HEADER, gfp);
			if (!nskb)
				goto nomem;

			/* Make sure the outbound skb has enough header
			 * room reserved.
			 */
			skb_reserve(nskb, packet->overhead + MAX_HEADER);
		} else {
			nskb = head;
		}

		/**
		 * 3.2  Chunk Field Descriptions
		 *
		 * The total length of a chunk (including Type, Length and
		 * Value fields) MUST be a multiple of 4 bytes.  If the length
		 * of the chunk is not a multiple of 4 bytes, the sender MUST
		 * pad the chunk with all zero bytes and this padding is not
		 * included in the chunk length field.  The sender should
		 * never pad with more than 3 bytes.
		 *
		 * [This whole comment explains SCTP_PAD4() below.]
		 */

		pkt_size -= packet->overhead;
		list_for_each_entry_safe(chunk, tmp, &packet->chunk_list, list) {
			list_del_init(&chunk->list);
			if (sctp_chunk_is_data(chunk)) {
				/* 6.3.1 C4) When data is in flight and when allowed
				 * by rule C5, a new RTT measurement MUST be made each
				 * round trip.  Furthermore, new RTT measurements
				 * SHOULD be made no more than once per round-trip
				 * for a given destination transport address.
				 */

				if (!chunk->resent && !tp->rto_pending) {
					chunk->rtt_in_progress = 1;
					tp->rto_pending = 1;
				}

				has_data = 1;
			}

			padding = SCTP_PAD4(chunk->skb->len) - chunk->skb->len;
			if (padding)
				memset(skb_put(chunk->skb, padding), 0, padding);

			/* if this is the auth chunk that we are adding,
			 * store pointer where it will be added and put
			 * the auth into the packet.
			 */
			if (chunk == packet->auth)
				auth = skb_tail_pointer(nskb);

			memcpy(skb_put(nskb, chunk->skb->len),
			       chunk->skb->data, chunk->skb->len);

			pr_debug("*** Chunk:%p[%s] %s 0x%x, length:%d, chunk->skb->len:%d, rtt_in_progress:%d\n",
				 chunk,
				 sctp_cname(SCTP_ST_CHUNK(chunk->chunk_hdr->type)),
				 chunk->has_tsn ? "TSN" : "No TSN",
				 chunk->has_tsn ? ntohl(chunk->subh.data_hdr->tsn) : 0,
				 ntohs(chunk->chunk_hdr->length), chunk->skb->len,
				 chunk->rtt_in_progress);

			/* If this is a control chunk, this is our last
			 * reference. Free data chunks after they've been
			 * acknowledged or have failed.
			 * Re-queue auth chunks if needed.
			 */
			pkt_size -= SCTP_PAD4(chunk->skb->len);

			if (!sctp_chunk_is_data(chunk) && chunk != packet->auth)
				sctp_chunk_free(chunk);

			if (!pkt_size)
				break;
		}

		/* SCTP-AUTH, Section 6.2
		 *    The sender MUST calculate the MAC as described in RFC2104 [2]
		 *    using the hash function H as described by the MAC Identifier and
		 *    the shared association key K based on the endpoint pair shared key
		 *    described by the shared key identifier.  The 'data' used for the
		 *    computation of the AUTH-chunk is given by the AUTH chunk with its
		 *    HMAC field set to zero (as shown in Figure 6) followed by all
		 *    chunks that are placed after the AUTH chunk in the SCTP packet.
		 */
		if (auth)
			sctp_auth_calculate_hmac(asoc, nskb,
						 (struct sctp_auth_chunk *)auth,
						 gfp);

		if (packet->auth) {
			if (!list_empty(&packet->chunk_list)) {
				/* We will generate more packets, so re-queue
				 * auth chunk.
				 */
				list_add(&packet->auth->list,
					 &packet->chunk_list);
			} else {
				sctp_chunk_free(packet->auth);
				packet->auth = NULL;
			}
		}

		if (!gso)
			break;

		if (skb_gro_receive(&head, nskb)) {
			kfree_skb(nskb);
			goto nomem;
		}
		nskb = NULL;
		if (WARN_ON_ONCE(skb_shinfo(head)->gso_segs >=
				 sk->sk_gso_max_segs))
			goto nomem;
	} while (!list_empty(&packet->chunk_list));

	/* 2) Calculate the Adler-32 checksum of the whole packet,
	 *    including the SCTP common header and all the
	 *    chunks.
	 *
	 * Note: Adler-32 is no longer applicable, as has been replaced
	 * by CRC32-C as described in <draft-ietf-tsvwg-sctpcsum-02.txt>.
	 *
	 * If it's a GSO packet, it's postponed to sctp_skb_segment.
	 */
	if (!sctp_checksum_disable || gso) {
		if (!gso && (!(dst->dev->features & NETIF_F_SCTP_CRC) ||
			     dst_xfrm(dst) || packet->ipfragok)) {
			sh->checksum = sctp_compute_cksum(head, 0);
		} else {
			/* no need to seed pseudo checksum for SCTP */
			head->ip_summed = CHECKSUM_PARTIAL;
			head->csum_start = skb_transport_header(head) - head->head;
			head->csum_offset = offsetof(struct sctphdr, checksum);
		}
	}

	/* IP layer ECN support
	 * From RFC 2481
	 *  "The ECN-Capable Transport (ECT) bit would be set by the
	 *   data sender to indicate that the end-points of the
	 *   transport protocol are ECN-capable."
	 *
	 * Now setting the ECT bit all the time, as it should not cause
	 * any problems protocol-wise even if our peer ignores it.
	 *
	 * Note: The works for IPv6 layer checks this bit too later
	 * in transmission.  See IP6_ECN_flow_xmit().
	 */
	tp->af_specific->ecn_capable(sk);

	/* Set up the IP options.  */
	/* BUG: not implemented
	 * For v4 this all lives somewhere in sk->sk_opt...
	 */

	/* Dump that on IP!  */
	if (asoc) {
		asoc->stats.opackets += pktcount;
		if (asoc->peer.last_sent_to != tp)
			/* Considering the multiple CPU scenario, this is a
			 * "correcter" place for last_sent_to.  --xguo
			 */
			asoc->peer.last_sent_to = tp;
	}

	if (has_data) {
		struct timer_list *timer;
		unsigned long timeout;

		/* Restart the AUTOCLOSE timer when sending data. */
		if (sctp_state(asoc, ESTABLISHED) &&
		    asoc->timeouts[SCTP_EVENT_TIMEOUT_AUTOCLOSE]) {
			timer = &asoc->timers[SCTP_EVENT_TIMEOUT_AUTOCLOSE];
			timeout = asoc->timeouts[SCTP_EVENT_TIMEOUT_AUTOCLOSE];

			if (!mod_timer(timer, jiffies + timeout))
				sctp_association_hold(asoc);
		}
	}

	pr_debug("***sctp_transmit_packet*** skb->len:%d\n", head->len);

	if (gso) {
		/* Cleanup our debris for IP stacks */
		memset(head->cb, 0, max(sizeof(struct inet_skb_parm),
					sizeof(struct inet6_skb_parm)));

		skb_shinfo(head)->gso_segs = pktcount;
		skb_shinfo(head)->gso_size = GSO_BY_FRAGS;

		/* We have to refresh this in case we are xmiting to
		 * more than one transport at a time
		 */
		rcu_read_lock();
		if (__sk_dst_get(sk) != tp->dst) {
			dst_hold(tp->dst);
			sk_setup_caps(sk, tp->dst);
		}
		rcu_read_unlock();
	}
	head->ignore_df = packet->ipfragok;
	tp->af_specific->sctp_xmit(head, tp);
	goto out;

nomem:
	if (packet->auth && list_empty(&packet->auth->list))
		sctp_chunk_free(packet->auth);

nodst:
	/* FIXME: Returning the 'err' will effect all the associations
	 * associated with a socket, although only one of the paths of the
	 * association is unreachable.
	 * The real failure of a transport or association can be passed on
	 * to the user via notifications. So setting this error may not be
	 * required.
	 */
	 /* err = -EHOSTUNREACH; */
	kfree_skb(head);

err:
	list_for_each_entry_safe(chunk, tmp, &packet->chunk_list, list) {
		list_del_init(&chunk->list);
		if (!sctp_chunk_is_data(chunk))
			sctp_chunk_free(chunk);
	}

out:
	sctp_packet_reset(packet);
	return err;
}

/********************************************************************
 * 2nd Level Abstractions
 ********************************************************************/

/* This private function check to see if a chunk can be added */
static sctp_xmit_t sctp_packet_can_append_data(struct sctp_packet *packet,
					   struct sctp_chunk *chunk)
{
	size_t datasize, rwnd, inflight, flight_size;
	struct sctp_transport *transport = packet->transport;
	struct sctp_association *asoc = transport->asoc;
	struct sctp_outq *q = &asoc->outqueue;

	/* RFC 2960 6.1  Transmission of DATA Chunks
	 *
	 * A) At any given time, the data sender MUST NOT transmit new data to
	 * any destination transport address if its peer's rwnd indicates
	 * that the peer has no buffer space (i.e. rwnd is 0, see Section
	 * 6.2.1).  However, regardless of the value of rwnd (including if it
	 * is 0), the data sender can always have one DATA chunk in flight to
	 * the receiver if allowed by cwnd (see rule B below).  This rule
	 * allows the sender to probe for a change in rwnd that the sender
	 * missed due to the SACK having been lost in transit from the data
	 * receiver to the data sender.
	 */

	rwnd = asoc->peer.rwnd;
	inflight = q->outstanding_bytes;
	flight_size = transport->flight_size;

	datasize = sctp_data_size(chunk);

	if (datasize > rwnd && inflight > 0)
		/* We have (at least) one data chunk in flight,
		 * so we can't fall back to rule 6.1 B).
		 */
		return SCTP_XMIT_RWND_FULL;

	/* RFC 2960 6.1  Transmission of DATA Chunks
	 *
	 * B) At any given time, the sender MUST NOT transmit new data
	 * to a given transport address if it has cwnd or more bytes
	 * of data outstanding to that transport address.
	 */
	/* RFC 7.2.4 & the Implementers Guide 2.8.
	 *
	 * 3) ...
	 *    When a Fast Retransmit is being performed the sender SHOULD
	 *    ignore the value of cwnd and SHOULD NOT delay retransmission.
	 */
	if (chunk->fast_retransmit != SCTP_NEED_FRTX &&
	    flight_size >= transport->cwnd)
		return SCTP_XMIT_RWND_FULL;

	/* Nagle's algorithm to solve small-packet problem:
	 * Inhibit the sending of new chunks when new outgoing data arrives
	 * if any previously transmitted data on the connection remains
	 * unacknowledged.
	 */

	if (sctp_sk(asoc->base.sk)->nodelay)
		/* Nagle disabled */
		return SCTP_XMIT_OK;

	if (!sctp_packet_empty(packet))
		/* Append to packet */
		return SCTP_XMIT_OK;

	if (inflight == 0)
		/* Nothing unacked */
		return SCTP_XMIT_OK;

	if (!sctp_state(asoc, ESTABLISHED))
		return SCTP_XMIT_OK;

	/* Check whether this chunk and all the rest of pending data will fit
	 * or delay in hopes of bundling a full sized packet.
	 */
	if (chunk->skb->len + q->out_qlen >
		transport->pathmtu - packet->overhead - sizeof(sctp_data_chunk_t) - 4)
		/* Enough data queued to fill a packet */
		return SCTP_XMIT_OK;

	/* Don't delay large message writes that may have been fragmented */
	if (!chunk->msg->can_delay)
		return SCTP_XMIT_OK;

	/* Defer until all data acked or packet full */
	return SCTP_XMIT_DELAY;
}

/* This private function does management things when adding DATA chunk */
static void sctp_packet_append_data(struct sctp_packet *packet,
				struct sctp_chunk *chunk)
{
	struct sctp_transport *transport = packet->transport;
	size_t datasize = sctp_data_size(chunk);
	struct sctp_association *asoc = transport->asoc;
	u32 rwnd = asoc->peer.rwnd;

	/* Keep track of how many bytes are in flight over this transport. */
	transport->flight_size += datasize;

	/* Keep track of how many bytes are in flight to the receiver. */
	asoc->outqueue.outstanding_bytes += datasize;

	/* Update our view of the receiver's rwnd. */
	if (datasize < rwnd)
		rwnd -= datasize;
	else
		rwnd = 0;

	asoc->peer.rwnd = rwnd;
	/* Has been accepted for transmission. */
	if (!asoc->peer.prsctp_capable)
		chunk->msg->can_abandon = 0;
	sctp_chunk_assign_tsn(chunk);
	sctp_chunk_assign_ssn(chunk);
}

static sctp_xmit_t sctp_packet_will_fit(struct sctp_packet *packet,
					struct sctp_chunk *chunk,
					u16 chunk_len)
{
	size_t psize, pmtu, maxsize;
	sctp_xmit_t retval = SCTP_XMIT_OK;

	psize = packet->size;
	if (packet->transport->asoc)
		pmtu = packet->transport->asoc->pathmtu;
	else
		pmtu = packet->transport->pathmtu;

	/* Decide if we need to fragment or resubmit later. */
	if (psize + chunk_len > pmtu) {
		/* It's OK to fragment at IP level if any one of the following
		 * is true:
		 *	1. The packet is empty (meaning this chunk is greater
		 *	   the MTU)
		 *	2. The packet doesn't have any data in it yet and data
		 *	   requires authentication.
		 */
		if (sctp_packet_empty(packet) ||
		    (!packet->has_data && chunk->auth)) {
			/* We no longer do re-fragmentation.
			 * Just fragment at the IP layer, if we
			 * actually hit this condition
			 */
			packet->ipfragok = 1;
			goto out;
		}

		/* Similarly, if this chunk was built before a PMTU
		 * reduction, we have to fragment it at IP level now. So
		 * if the packet already contains something, we need to
		 * flush.
		 */
		maxsize = pmtu - packet->overhead;
		if (packet->auth)
			maxsize -= SCTP_PAD4(packet->auth->skb->len);
		if (chunk_len > maxsize)
			retval = SCTP_XMIT_PMTU_FULL;

		/* It is also okay to fragment if the chunk we are
		 * adding is a control chunk, but only if current packet
		 * is not a GSO one otherwise it causes fragmentation of
		 * a large frame. So in this case we allow the
		 * fragmentation by forcing it to be in a new packet.
		 */
		if (!sctp_chunk_is_data(chunk) && packet->has_data)
			retval = SCTP_XMIT_PMTU_FULL;

		if (psize + chunk_len > packet->max_size)
			/* Hit GSO/PMTU limit, gotta flush */
			retval = SCTP_XMIT_PMTU_FULL;

		if (!packet->transport->burst_limited &&
		    psize + chunk_len > (packet->transport->cwnd >> 1))
			/* Do not allow a single GSO packet to use more
			 * than half of cwnd.
			 */
			retval = SCTP_XMIT_PMTU_FULL;

		if (packet->transport->burst_limited &&
		    psize + chunk_len > (packet->transport->burst_limited >> 1))
			/* Do not allow a single GSO packet to use more
			 * than half of original cwnd.
			 */
			retval = SCTP_XMIT_PMTU_FULL;
		/* Otherwise it will fit in the GSO packet */
	}

out:
	return retval;
}
