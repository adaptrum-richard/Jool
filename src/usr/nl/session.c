#include "usr/nl/session.h"

#include <errno.h>
#include <netlink/genl/genl.h>
#include "usr/nl/attribute.h"

struct foreach_args {
	session_foreach_cb cb;
	void *args;
	bool done;
	struct session_entry_usr last;
};

static struct jool_result entry2attr(struct session_entry_usr *entry,
		struct nl_msg *msg)
{
	struct nlattr *root;

	root = nla_nest_start(msg, RA_SESSION_ENTRY);
	if (!root)
		goto nla_put_failure;

	if (nla_put_taddr6(msg, SEA_SRC6, &entry->src6))
		goto nla_put_failure;
	if (nla_put_taddr6(msg, SEA_DST6, &entry->dst6))
		goto nla_put_failure;
	if (nla_put_taddr4(msg, SEA_SRC4, &entry->src4))
		goto nla_put_failure;
	if (nla_put_taddr4(msg, SEA_DST4, &entry->dst4))
		goto nla_put_failure;
	NLA_PUT_U8(msg, SEA_PROTO, entry->proto);
	NLA_PUT_U8(msg, SEA_STATE, entry->state);
	NLA_PUT_U32(msg, SEA_EXPIRATION, entry->dying_time);

	nla_nest_end(msg, root);
	return result_success();

nla_put_failure:
	return packet_too_small();
}

static struct jool_result attr2entry(struct nlattr *attr,
		struct session_entry_usr *entry)
{
	struct nlattr *attrs[SEA_COUNT];
	struct jool_result result;

	result = jnla_parse_nested(attrs, SEA_MAX, attr, session_entry_policy);
	if (result.error)
		return result;

	result = nla_get_taddr6(attrs[SEA_SRC6], &entry->src6);
	if (result.error)
		return result;
	result = nla_get_taddr6(attrs[SEA_DST6], &entry->dst6);
	if (result.error)
		return result;
	result = nla_get_taddr4(attrs[SEA_SRC4], &entry->src4);
	if (result.error)
		return result;
	result = nla_get_taddr4(attrs[SEA_DST4], &entry->dst4);
	if (result.error)
		return result;
	entry->proto = nla_get_u8(attrs[SEA_PROTO]);
	entry->state = nla_get_u8(attrs[SEA_STATE]);
	entry->dying_time = nla_get_u32(attrs[SEA_EXPIRATION]);
	return result_success();
}

static struct jool_result handle_foreach_response(struct nl_msg *response,
		void *arg)
{
	struct foreach_args *args;
	struct genlmsghdr *ghdr;
	struct request_hdr *jhdr;
	struct nlattr *attr;
	int rem;
	struct session_entry_usr entry;
	struct jool_result result;

	args = arg;
	ghdr = genlmsg_hdr(nlmsg_hdr(response));

	foreach_entry(attr, ghdr, rem) {
		result = attr2entry(attr, &entry);
		if (result.error)
			return result;

		result = args->cb(&entry, args->args);
		if (result.error)
			return result;

		memcpy(&args->last, &entry, sizeof(entry));
	}

	jhdr = genlmsg_user_hdr(ghdr);
	args->done = !(jhdr->flags & HDRFLAGS_M);
	return result_success();
}

struct jool_result session_foreach(struct jool_socket *sk, char *iname,
		l4_protocol proto, session_foreach_cb cb, void *_args)
{
	struct nl_msg *msg;
	struct foreach_args args;
	struct jool_result result;
	bool first_request;

	args.cb = cb;
	args.args = _args;
	args.done = false;
	memset(&args.last, 0, sizeof(args.last));
	first_request = true;

	do {
		result = allocate_jool_nlmsg(sk, iname, JOP_SESSION_FOREACH, 0, &msg);
		if (result.error)
			return result;

		if (first_request) {
			result.error = nla_put_u8(msg, SEA_PROTO, proto);
			if (result.error)
				return packet_too_small();
			first_request = false;
		} else {
			result = entry2attr(&args.last, msg);
			if (result.error)
				return result;
		}

		result = netlink_request(sk, msg, handle_foreach_response, &args);
		if (result.error)
			return result;
	} while (!args.done);

	return result_success();
}

