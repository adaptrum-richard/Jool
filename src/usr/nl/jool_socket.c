#include "usr/nl/jool_socket.h"

#include <errno.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include "usr/nl/attribute.h"

/*
 * Assert we're compiling with libnl version >= 3.0
 *
 * Note: it looks like this shouldn't be here, since it's the configure script's
 * responsibility.
 * However, the configure script seems to fail to detect this properly on RedHat
 * (and maybe others).
 */
#if !defined(LIBNL_VER_NUM)
	#error "Missing LIBNL dependency (need at least version 3)."
#endif
#if LIBNL_VER_NUM < LIBNL_VER(3, 0)
	#error "Unsupported LIBNL library version number (< 3.0)."
#endif


struct response_cb {
	jool_response_cb cb;
	void *arg;
	struct jool_result result;
};

struct jool_result allocate_jool_nlmsg(struct jool_socket *socket,
		char const *iname, enum jool_operation op, __u8 flags,
		struct nl_msg **out)
{
	struct nl_msg *msg;
	struct joolnl_hdr *hdr;
	int error;

	error = iname_validate(iname, true);
	if (error)
		return result_from_error(error, INAME_VALIDATE_ERRMSG);

	msg = nlmsg_alloc();
	if (!msg)
		return result_from_enomem();

	hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, socket->genl_family,
			sizeof(struct joolnl_hdr), 0, op, 1);
	if (!hdr) {
		nlmsg_free(msg);
		return result_from_error(
			-EINVAL,
			"Cannot initialize Netlink headers (Unknown cause.)"
		);
	}

	init_request_hdr(hdr, socket->xt, iname, flags);
	*out = msg;
	return result_success();
}

static struct jool_result print_error_msg(struct nl_msg *response)
{
	static struct nla_policy error_policy[ERRA_COUNT] = {
		[ERRA_CODE] = { .type = NLA_U16 },
		[ERRA_MSG] = { .type = NLA_STRING },
	};
	struct nlattr *attrs[ERRA_COUNT];
	struct jool_result result;
	__u16 code;
	char *msg;

	result = jnla_parse_msg(response, attrs, ERRA_MAX, error_policy, false);
	if (result.error)
		return result;

	code = attrs[ERRA_CODE] ? nla_get_u16(attrs[ERRA_CODE]) : EINVAL;
	msg = attrs[ERRA_MSG] ? nla_get_string(attrs[ERRA_MSG]) : strerror(code);
	return result_from_error(
		code,
		"The kernel module returned error %u: %s", code, msg
	);
}

/*
 * Heads up:
 * Netlink wants this function to return either a negative error code or an enum
 * nl_cb_action.
 * Because NL_SKIP == EPERM and NL_STOP == ENOENT, you really need to mind the
 * sign of the result.
 */
static int response_handler(struct nl_msg *response, void *_args)
{
	struct response_cb *args;
	struct nlmsghdr *nhdr;
	struct joolnl_hdr *jhdr;
	int error;

	/* Also: arg->result needs to be set on all paths. */

	args = _args;
	nhdr = nlmsg_hdr(response);
	if (!genlmsg_valid_hdr(nhdr, sizeof(struct joolnl_hdr))) {
		args->result = result_from_error(
			-NLE_MSG_TOOSHORT,
			"The kernel module's response is too small."
		);
		goto end;
	}

	jhdr = genlmsg_user_hdr(genlmsg_hdr(nhdr));
	if (jhdr->flags & HDRFLAGS_ERROR) {
		args->result = print_error_msg(response);
		goto end;
	}

	args->result = args->cb
			? args->cb(response, args->arg)
			: result_success();
	/* Fall through */

end:
	error = args->result.error;
	return (error < 0) ? error : -error;
}

/**
 * @iname can be NULL. The kernel module will assume that the instance name is
 * "" (empty string).
 *
 * Consumes @msg, even on error.
 */
struct jool_result netlink_request(struct jool_socket *socket,
		struct nl_msg *msg, jool_response_cb cb, void *cb_arg)
{
	struct response_cb callback;
	struct jool_result result;

	callback.cb = cb;
	callback.arg = cb_arg;
	/* Clear out JRF_INITIALIZED and error code */
	memset(&callback.result, 0, sizeof(callback.result));

	result.error = nl_socket_modify_cb(socket->sk, NL_CB_MSG_IN, NL_CB_CUSTOM,
			response_handler, &callback);
	if (result.error < 0) {
		nlmsg_free(msg);
		return result_from_error(
			result.error,
			"Could not register response handler: %s\n",
			nl_geterror(result.error)
		);
	}

	result.error = nl_send_auto(socket->sk, msg);
	nlmsg_free(msg);
	if (result.error < 0) {
		return result_from_error(
			result.error,
			"Could not dispatch the request to kernelspace: %s",
			nl_geterror(result.error)
		);
	}

	result.error = nl_recvmsgs_default(socket->sk);
	if (result.error < 0) {
		if ((callback.result.flags & JRF_INITIALIZED)
				&& callback.result.error) {
			/* nl_recvmsgs_default() failed during our callback */
			return callback.result;
		}

		/* nl_recvmsgs_default() failed before or after our callback */
		return result_from_error(
			result.error,
			"Error receiving the kernel module's response: %s",
			nl_geterror(result.error)
		);
	}

	return result_success();
}

/**
 * Contract: The result will contain 0 on success, -ESRCH on module likely not
 * modprobed, else -EINVAL.
 */
struct jool_result netlink_setup(struct jool_socket *socket, xlator_type xt)
{
	int error;

	error = xt_validate(xt);
	if (error)
		return result_from_error(error, XT_VALIDATE_ERRMSG);

	socket->sk = nl_socket_alloc();
	if (!socket->sk) {
		return result_from_error(
			-EINVAL,
			"Netlink socket allocation failure (Unknown cause)"
		);
	}

	/*
	 * For normal requests, we handle ACKs ourselves. The reason is that
	 * Netlink ACK errors do not contain the friendly error string, so
	 * they're useless to us.
	 * https://github.com/NICMx/Jool/issues/169
	 *
	 * In joold's case, it's because ACKs would only slow us down.
	 * We use UDP in the network, so we're assuming best-effort anyway.
	 */
	nl_socket_disable_auto_ack(socket->sk);

	error = genl_connect(socket->sk);
	if (error) {
		nl_socket_free(socket->sk);
		return result_from_error(
			-EINVAL,
			"Could not open the socket to kernelspace: %s",
			nl_geterror(error)
		);
	}

	socket->xt = xt;
	socket->genl_family = genl_ctrl_resolve(socket->sk, GNL_JOOL_FAMILY);
	if (socket->genl_family < 0) {
		nl_socket_free(socket->sk);
		return result_from_error(
			-ESRCH,
			"Jool's socket family doesn't seem to exist.\n"
			"(This probably means Jool hasn't been modprobed.)\n"
			"Netlink error message: %s",
			nl_geterror(socket->genl_family)
		);
	}

	return result_success();
}

void netlink_teardown(struct jool_socket *socket)
{
	nl_close(socket->sk);
	nl_socket_free(socket->sk);
}
