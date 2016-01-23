#include "nat64/common/types.h"
#include "nat64/common/config.h"
#include "nat64/mod/common/packet.h"
#include "nat64/mod/stateful/compute_outgoing_tuple.h"
#include "nat64/mod/stateful/determine_incoming_tuple.h"
#include "nat64/mod/stateful/filtering_and_updating.h"
#include "nat64/mod/stateful/fragment_db.h"
#include "nat64/mod/stateful/pool4/db.h"
#include "nat64/mod/stateful/bib/db.h"
#include "nat64/mod/stateful/bib/static_routes.h"
#include "nat64/mod/stateful/session/db.h"

/**
 * @file
 * NAT64-specific functions, as linked by SIIT code.
 *
 * These are all supposed to be unreachable code, so they're very noisy on the
 * kernel log.
 */

static int fail(const char *function_name)
{
	WARN(true, "%s() was called from SIIT code.", function_name);
	return -EINVAL;
}

verdict determine_in_tuple(struct xlation *state)
{
	fail(__func__);
	return VERDICT_DROP;
}

verdict filtering_and_updating(struct xlation *state)
{
	fail(__func__);
	return VERDICT_DROP;
}

verdict compute_out_tuple(struct xlation *state)
{
	fail(__func__);
	return VERDICT_DROP;
}

int pool4db_init(struct pool4 **pool, unsigned int capacity)
{
	return fail(__func__);
}

void pool4db_get(struct pool4 *pool)
{
	fail(__func__);
}

void pool4db_put(struct pool4 *pool)
{
	fail(__func__);
}

int pool4db_add(struct pool4 *pool, const __u32 mark, enum l4_protocol proto,
		struct ipv4_prefix *prefix, struct port_range *ports)
{
	return fail(__func__);
}

int pool4db_rm(struct pool4 *pool, const __u32 mark, enum l4_protocol proto,
		struct ipv4_prefix *prefix, struct port_range *ports)
{
	return fail(__func__);
}

int pool4db_flush(struct pool4 *pool)
{
	return fail(__func__);
}

int pool4db_foreach_sample(struct pool4 *pool,
		int (*cb)(struct pool4_sample *, void *), void *arg,
		struct pool4_sample *offset)
{
	return fail(__func__);
}

void pool4db_count(struct pool4 *pool, __u32 *tables, __u64 *samples,
		__u64 *taddrs)
{
	fail(__func__);
}

bool pool4db_is_empty(struct pool4 *pool)
{
	fail(__func__);
	return true;
}

int bibdb_init(struct bib **db)
{
	return fail(__func__);
}

void bibdb_get(struct bib *db)
{
	fail(__func__);
}

void bibdb_put(struct bib *db)
{
	fail(__func__);
}

void bibdb_delete_taddr4s(struct bib *db, const struct ipv4_prefix *prefix,
		struct port_range *ports)
{
	fail(__func__);
}

void bibdb_flush(struct bib *db)
{
	fail(__func__);
}

int bibdb_foreach(struct bib *db, const l4_protocol proto,
		int (*func)(struct bib_entry *, void *), void *arg,
		const struct ipv4_transport_addr *offset)
{
	return fail(__func__);
}

int bibdb_count(struct bib *db, const l4_protocol proto, __u64 *result)
{
	return fail(__func__);
}

int add_static_route(struct xlator *jool, struct request_bib *req)
{
	return fail(__func__);
}

int delete_static_route(struct xlator *jool, struct request_bib *request)
{
	return fail(__func__);
}

int sessiondb_init(struct sessiondb **db)
{
	return fail(__func__);
}

void sessiondb_get(struct sessiondb *db)
{
	fail(__func__);
}

void sessiondb_put(struct sessiondb *db)
{
	fail(__func__);
}

void sessiondb_delete_taddr6s(struct sessiondb *db, struct ipv6_prefix *prefix)
{
	fail(__func__);
}

void sessiondb_delete_taddr4s(struct sessiondb *db, struct ipv4_prefix *prefix,
		struct port_range *ports)
{
	fail(__func__);
}

void sessiondb_flush(struct sessiondb *db)
{
	fail(__func__);
}

int sessiondb_foreach(struct sessiondb *db, l4_protocol proto,
		int (*func)(struct session_entry *, void *), void *arg,
		struct ipv4_transport_addr *offset_remote,
		struct ipv4_transport_addr *offset_local)
{
	return fail(__func__);
}

int sessiondb_count(struct sessiondb *db, l4_protocol proto, __u64 *result)
{
	return fail(__func__);
}

void sessiondb_update_timers(struct sessiondb *db)
{
	fail(__func__);
}

verdict fragdb_handle(struct packet *pkt)
{
	fail(__func__);
	return VERDICT_DROP;
}