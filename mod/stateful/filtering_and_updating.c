#include "nat64/mod/stateful/filtering_and_updating.h"

#include "nat64/common/str_utils.h"
#include "nat64/mod/common/config.h"
#include "nat64/mod/common/icmp_wrapper.h"
#include "nat64/mod/common/pool6.h"
#include "nat64/mod/common/rfc6052.h"
#include "nat64/mod/common/stats.h"
#include "nat64/mod/stateful/joold.h"
#include "nat64/mod/stateful/pool4/db.h"
#include "nat64/mod/stateful/bib/db.h"

#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/icmpv6.h>
#include <net/tcp.h>
#include <net/icmp.h>

enum session_fate tcp_expired_cb(struct session_entry *session, void *arg)
{
	switch (session->state) {
	case ESTABLISHED:
		session->state = TRANS;
		session->update_time = jiffies;
		return FATE_PROBE;

	case V4_INIT:
	case V6_INIT:
	case V4_FIN_RCV:
	case V6_FIN_RCV:
	case V4_FIN_V6_FIN_RCV:
	case TRANS:
		session->state = CLOSED;
		return FATE_RM;

	case CLOSED:
		/* Closed sessions must not be stored; this is an error. */
		WARN(true, "Closed state found; removing session entry.");
		return FATE_RM;
	}

	WARN(true, "Unknown state found (%d); removing session entry.",
			session->state);
	return FATE_RM;
}

static void log_entries(struct bib_session *entries)
{
	struct session_entry *session = &entries->session;

	if (entries->bib_set) {
		log_debug("BIB entry: %pI6c#%u - %pI4#%u (%s)",
				&session->src6.l3, session->src6.l4,
				&session->src6.l3, session->src4.l4,
				l4proto_to_string(session->proto));
	} else {
		log_debug("BIB entry: None");
	}

	if (entries->session_set) {
		log_debug("Session entry: %pI6c#%u - %pI6c#%u | %pI4#%u - %pI4#%u (%s)",
				&session->src6.l3, session->src6.l4,
				&session->dst6.l3, session->dst6.l4,
				&session->src4.l3, session->src4.l4,
				&session->dst4.l3, session->dst4.l4,
				l4proto_to_string(session->proto));
	} else {
		log_debug("Session entry: None");
	}
}

static verdict succeed(struct xlation *state)
{
	log_entries(&state->entries);

	/*
	 * Sometimes the session doesn't change as a result of the state
	 * machine's schemes.
	 * No state change, no timeout change, no update time change.
	 *
	 * One might argue that we shouldn't joold the session in those cases.
	 * It's a lot more trouble than it's worth:
	 *
	 * - Calling joold_add() on the TCP SM state functions is incorrect
	 *   because the session's update_time and expirer haven't been updated
	 *   by that point. So what gets synchronizes is half-baked data.
	 * - Calling joold_add() on decide_fate() is a freaking mess because
	 *   we'd need to send the xlator and a boolean (indicating whether this
	 *   is packet or timer context) to it and all intermediate functions,
	 *   and these functions all already have too many arguments as it is.
	 *   It's bad design anyway; the session module belongs to a layer that
	 *   shouldn't be aware of the xlator.
	 * - These special no-changes cases are rare.
	 *
	 * So let's simplify everything by just joold_add()ing here.
	 */
	if (state->entries.session_set) {
		joold_add(state->jool.nat64.joold, &state->entries.session,
				state->jool.nat64.bib);
	}
	return VERDICT_CONTINUE;
}

static verdict breakdown(struct xlation *state)
{
	inc_stats(&state->in, IPSTATS_MIB_INDISCARDS);
	return VERDICT_DROP;
}

/**
 * This is just a wrapper. Its sole intent is to minimize mess below.
 */
static int xlat_dst_6to4(struct xlation *state,
		struct ipv4_transport_addr *dst4)
{
	dst4->l4 = state->in.tuple.dst.addr6.l4;
	return rfc6052_6to4(state->jool.pool6, &state->in.tuple.dst.addr6.l3,
			&dst4->l3);
}

/**
 * This is just a wrapper. Its sole intent is to minimize mess below.
 */
static int find_mask_domain(struct xlation *state, struct mask_domain **masks)
{
	*masks = mask_domain_find(state->jool.nat64.pool4,
			&state->in.tuple,
			state->jool.global->cfg.nat64.f_args,
			state->in.skb->mark);
	return (*masks) ? 0 : -EINVAL;
}

/**
 * Assumes that "tuple" represents a IPv6-UDP or ICMP packet, and filters and
 * updates based on it.
 *
 * This is RFC 6146, first halves of both sections 3.5.1 and 3.5.3.
 *
 * @pkt: tuple's packet. This is actually only used for error reporting.
 * @tuple: summary of the packet Jool is currently translating.
 */
static verdict ipv6_simple(struct xlation *state)
{
	struct ipv4_transport_addr dst4;
	struct mask_domain *masks;
	int error;

	if (xlat_dst_6to4(state, &dst4))
		return breakdown(state);
	if (find_mask_domain(state, &masks))
		return breakdown(state);

	error = bib_add6(state->jool.nat64.bib, masks, &state->in.tuple, &dst4,
			&state->entries);
	mask_domain_put(masks);

	switch (error) {
	case 0:
	case -EEXIST:
		return succeed(state);
	default:
		return breakdown(state);
	}
}

/**
 * Assumes that "tuple" represents a IPv4-UDP or ICMP packet, and filters and
 * updates based on it.
 *
 * This is RFC 6146, second halves of both sections 3.5.1 and 3.5.3.
 *
 * @pkt skb tuple's packet. This is actually only used for error reporting.
 * @tuple4 tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static verdict ipv4_simple(struct xlation *state)
{
	struct ipv4_transport_addr *src4 = &state->in.tuple.src.addr4;
	struct ipv6_transport_addr dst6;
	int error;

	error = rfc6052_4to6(state->jool.pool6, &src4->l3, &dst6.l3);
	if (error)
		return breakdown(state); /* TODO error msg? */
	dst6.l4 = src4->l4;

	error = bib_add4(state->jool.nat64.bib, &dst6, &state->in.tuple,
			&state->entries);

	switch (error) {
	case 0:
	case -EEXIST:
		return succeed(state);
	case -ESRCH:
		/* TODO breakdown? */
		log_debug("There is no BIB entry for the IPv4 packet.");
		inc_stats(&state->in, IPSTATS_MIB_INNOROUTES);
		return VERDICT_ACCEPT;
	case -EPERM:
		log_debug("Packet was blocked by address-dependent filtering.");
		icmp64_send(&state->in, ICMPERR_FILTER, 0);
		inc_stats(&state->in, IPSTATS_MIB_INDISCARDS);
		return VERDICT_DROP;
	default:
		log_debug("Errcode %d while finding a BIB entry.", error);
		inc_stats(&state->in, IPSTATS_MIB_INDISCARDS);
		icmp64_send(&state->in, ICMPERR_ADDR_UNREACHABLE, 0);
		return breakdown(state);
	}
}

/**
 * Filtering and updating during the V4 INIT state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_init_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_l3_proto(pkt) == L3PROTO_IPV6 && pkt_tcp_hdr(pkt)->syn) {
		session->state = ESTABLISHED;
		return FATE_TIMER_EST;
	}

	return FATE_PRESERVE;
}

/**
 * Filtering and updating during the V6 INIT state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v6_init_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_tcp_hdr(pkt)->syn) {
		switch (pkt_l3_proto(pkt)) {
		case L3PROTO_IPV4:
			session->state = ESTABLISHED;
			return FATE_TIMER_EST;
		case L3PROTO_IPV6:
			return FATE_TIMER_TRANS;
		}
	}

	return FATE_PRESERVE;
}

/**
 * Filtering and updating during the ESTABLISHED state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_established_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_tcp_hdr(pkt)->fin) {
		switch (pkt_l3_proto(pkt)) {
		case L3PROTO_IPV4:
			session->state = V4_FIN_RCV;
			break;
		case L3PROTO_IPV6:
			session->state = V6_FIN_RCV;
			break;
		}
		return FATE_PRESERVE;

	} else if (pkt_tcp_hdr(pkt)->rst) {
		session->state = TRANS;
		return FATE_TIMER_TRANS;
	}

	return FATE_TIMER_EST;
}

/**
 * Filtering and updating during the V4 FIN RCV state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_fin_rcv_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_l3_proto(pkt) == L3PROTO_IPV6 && pkt_tcp_hdr(pkt)->fin) {
		session->state = V4_FIN_V6_FIN_RCV;
		return FATE_TIMER_TRANS;
	}

	return FATE_TIMER_EST;
}

/**
 * Filtering and updating during the V6 FIN RCV state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v6_fin_rcv_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_l3_proto(pkt) == L3PROTO_IPV4 && pkt_tcp_hdr(pkt)->fin) {
		session->state = V4_FIN_V6_FIN_RCV;
		return FATE_TIMER_TRANS;
	}

	return FATE_TIMER_EST;
}

/**
 * Filtering and updating during the V6 FIN + V4 FIN RCV state of the TCP state
 * machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_fin_v6_fin_rcv_state(void)
{
	return FATE_PRESERVE; /* Only the timeout can change this state. */
}

/**
 * Filtering and updating done during the TRANS state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_trans_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (!pkt_tcp_hdr(pkt)->rst) {
		session->state = ESTABLISHED;
		return FATE_TIMER_EST;
	}

	return FATE_PRESERVE;
}

static enum session_fate tcp_state_machine(struct session_entry *session,
		void *arg)
{
	switch (session->state) {
	case V4_INIT:
		return tcp_v4_init_state(session, arg);
	case V6_INIT:
		return tcp_v6_init_state(session, arg);
	case ESTABLISHED:
		return tcp_established_state(session, arg);
	case V4_FIN_RCV:
		return tcp_v4_fin_rcv_state(session, arg);
	case V6_FIN_RCV:
		return tcp_v6_fin_rcv_state(session, arg);
	case V4_FIN_V6_FIN_RCV:
		return tcp_v4_fin_v6_fin_rcv_state();
	case TRANS:
		return tcp_trans_state(session, arg);
	case CLOSED:
		break; /* Closed sessions are not supposed to be stored. */
	}

	WARN(true, "Invalid state found: %u.", session->state);
	return FATE_RM;
}

/**
 * IPv6 half of RFC 6146 section 3.5.2.
 */
static verdict ipv6_tcp(struct xlation *state)
{
	struct ipv4_transport_addr dst4;
	struct collision_cb cb;
	struct mask_domain *masks;
	verdict verdict;

	if (xlat_dst_6to4(state, &dst4))
		return breakdown(state);
	if (find_mask_domain(state, &masks))
		return breakdown(state);

	cb.cb = tcp_state_machine;
	cb.arg = state;
	verdict = bib_add_tcp6(state->jool.nat64.bib, masks, &dst4, &state->in,
			&cb, &state->entries);

	mask_domain_put(masks);

	switch (verdict) {
	case VERDICT_CONTINUE:
		return succeed(state);
	case VERDICT_DROP:
		return breakdown(state);
	case VERDICT_STOLEN:
	case VERDICT_ACCEPT:
		break;
	}

	return verdict;
}

/**
 * IPv4 half of RFC 6146 section 3.5.2.
 */
static verdict ipv4_tcp(struct xlation *state)
{
	struct ipv4_transport_addr *src4 = &state->in.tuple.src.addr4;
	struct ipv6_transport_addr dst6;
	struct collision_cb cb;
	verdict verdict;
	int error;

	error = rfc6052_4to6(state->jool.pool6, &src4->l3, &dst6.l3);
	if (error)
		return breakdown(state); /* TODO error msg? */
	dst6.l4 = src4->l4;

	cb.cb = tcp_state_machine;
	cb.arg = state;
	verdict = bib_add_tcp4(state->jool.nat64.bib, &dst6, &state->in, &cb,
			&state->entries);

	switch (verdict) {
	case VERDICT_CONTINUE:
		return succeed(state);
	case VERDICT_DROP:
		return breakdown(state);
	case VERDICT_STOLEN:
	case VERDICT_ACCEPT:
		break;
	}

	return verdict;
}

/**
 * filtering_and_updating - Main F&U routine. Decides if "skb" should be
 * processed, updating binding and session information.
 */
verdict filtering_and_updating(struct xlation *state)
{
	struct packet *in = &state->in;
	struct ipv6hdr *hdr_ip6;
	verdict result = VERDICT_CONTINUE;

	log_debug("Step 2: Filtering and Updating");

	switch (pkt_l3_proto(in)) {
	case L3PROTO_IPV6:
		/* Get rid of hairpinning loops and unwanted packets. */
		hdr_ip6 = pkt_ip6_hdr(in);
		if (pool6_contains(state->jool.pool6, &hdr_ip6->saddr)) {
			log_debug("Hairpinning loop. Dropping...");
			inc_stats(in, IPSTATS_MIB_INADDRERRORS);
			return VERDICT_DROP;
		}
		if (!pool6_contains(state->jool.pool6, &hdr_ip6->daddr)) {
			log_debug("Packet does not belong to pool6.");
			return VERDICT_ACCEPT;
		}

		/* ICMP errors should not be filtered or affect the tables. */
		if (pkt_is_icmp6_error(in)) {
			log_debug("Packet is ICMPv6 error; skipping step...");
			return VERDICT_CONTINUE;
		}
		break;
	case L3PROTO_IPV4:
		/* Get rid of unexpected packets */
		if (!pool4db_contains(state->jool.nat64.pool4, state->jool.ns,
				in->tuple.l4_proto, &in->tuple.dst.addr4)) {
			log_debug("Packet does not belong to pool4.");
			return VERDICT_ACCEPT;
		}

		/* ICMP errors should not be filtered or affect the tables. */
		if (pkt_is_icmp4_error(in)) {
			log_debug("Packet is ICMPv4 error; skipping step...");
			return VERDICT_CONTINUE;
		}
		break;
	}

	switch (pkt_l4_proto(in)) {
	case L4PROTO_UDP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			result = ipv6_simple(state);
			break;
		case L3PROTO_IPV4:
			result = ipv4_simple(state);
			break;
		}
		break;

	case L4PROTO_TCP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			result = ipv6_tcp(state);
			break;
		case L3PROTO_IPV4:
			result = ipv4_tcp(state);
			break;
		}
		break;

	case L4PROTO_ICMP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			if (state->jool.global->cfg.nat64.drop_icmp6_info) {
				log_debug("Packet is ICMPv6 info (ping); dropping due to policy.");
				inc_stats(in, IPSTATS_MIB_INDISCARDS);
				return VERDICT_DROP;
			}

			result = ipv6_simple(state);
			break;
		case L3PROTO_IPV4:
			result = ipv4_simple(state);
			break;
		}
		break;

	case L4PROTO_OTHER:
		WARN(true, "Unknown layer 4 protocol: %d", pkt_l4_proto(in));
		result = VERDICT_DROP;
		break;
	}

	log_debug("Done: Step 2.");
	return result;
}
