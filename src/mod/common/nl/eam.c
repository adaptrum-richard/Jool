#include "mod/common/nl/eam.h"

#include "common/types.h"
#include "mod/common/log.h"
#include "mod/common/xlator.h"
#include "mod/common/nl/attribute.h"
#include "mod/common/nl/nl_common.h"
#include "mod/common/nl/nl_core.h"
#include "mod/common/db/eam.h"

static int serialize_eam_entry(struct eamt_entry const *entry, void *arg)
{
	return jnla_put_eam(arg, LA_ENTRY, entry) ? 1 : 0;
}

int handle_eamt_foreach(struct sk_buff *skb, struct genl_info *info)
{
	struct xlator jool;
	struct jool_response response;
	struct ipv4_prefix offset, *offset_ptr;
	int error;

	error = request_handle_start(info, XT_SIIT, &jool);
	if (error)
		goto end;
	error = jresponse_init(&response, info);
	if (error)
		goto revert_start;

	offset_ptr = NULL;
	if (info->attrs[RA_OFFSET]) {
		error = jnla_get_prefix4(info->attrs[RA_OFFSET], "Iteration offset", &offset);
		if (error)
			goto revert_response;
		offset_ptr = &offset;
	}

	error = eamt_foreach(jool.siit.eamt, serialize_eam_entry, response.skb, offset_ptr);
	if (error < 0) {
		jresponse_cleanup(&response);
		goto revert_response;
	}

	error = jresponse_send_array(&response, error);
	if (error)
		goto revert_response;

	request_handle_end(&jool);
	return 0;

revert_response:
	jresponse_cleanup(&response);
revert_start:
	request_handle_end(&jool);
end:
	return jresponse_send_simple(info, error);
}

int handle_eamt_add(struct sk_buff *skb, struct genl_info *info)
{
	struct xlator jool;
	struct eamt_entry addend;
	int error;

	log_debug("Adding EAMT entry.");

	error = request_handle_start(info, XT_SIIT, &jool);
	if (error)
		goto end;

	error = jnla_get_eam(info->attrs[RA_OPERAND], "Operand", &addend);
	if (error)
		goto revert_start;

	error = eamt_add(jool.siit.eamt, &addend,
			get_jool_hdr(info)->flags & HDRFLAGS_FORCE);
revert_start:
	request_handle_end(&jool);
end:
	return jresponse_send_simple(info, error);
}

int handle_eamt_rm(struct sk_buff *skb, struct genl_info *info)
{
	struct xlator jool;
	struct nlattr *attrs[EA_COUNT];
	struct ipv6_prefix prefix6, *prefix6_ptr;
	struct ipv4_prefix prefix4, *prefix4_ptr;
	int error;

	log_debug("Removing EAMT entry.");

	error = request_handle_start(info, XT_SIIT, &jool);
	if (error)
		goto end;

	if (!info->attrs[RA_OPERAND]) {
		log_err("The request is missing the 'Operand' attribute.");
		error = -EINVAL;
		goto revert_start;
	}
	error = nla_parse_nested(attrs, EA_MAX, info->attrs[RA_OPERAND], eam_policy, NULL);
	if (error) {
		log_err("The 'EAMT' attribute is malformed.");
		goto revert_start;
	}

	if (!attrs[EA_PREFIX6] && !attrs[EA_PREFIX4]) {
		log_err("The request contains no prefixes.");
		error = -ENOENT;
		goto revert_start;
	}
	prefix6_ptr = NULL;
	if (attrs[EA_PREFIX6]) {
		error = jnla_get_prefix6(attrs[EA_PREFIX6], "IPv6 prefix", &prefix6);
		if (error)
			goto revert_start;
		prefix6_ptr = &prefix6;
	}
	prefix4_ptr = NULL;
	if (attrs[EA_PREFIX4]) {
		error = jnla_get_prefix4(attrs[EA_PREFIX4], "IPv4 prefix", &prefix4);
		if (error)
			goto revert_start;
		prefix4_ptr = &prefix4;
	}

	error = eamt_rm(jool.siit.eamt, prefix6_ptr, prefix4_ptr);
revert_start:
	request_handle_end(&jool);
end:
	return jresponse_send_simple(info, error);
}

int handle_eamt_flush(struct sk_buff *skb, struct genl_info *info)
{
	struct xlator jool;
	int error;

	log_debug("Flushing EAM table.");

	error = request_handle_start(info, XT_SIIT, &jool);
	if (error)
		goto end;

	eamt_flush(jool.siit.eamt);
	request_handle_end(&jool);
end:
	return jresponse_send_simple(info, error);
}
