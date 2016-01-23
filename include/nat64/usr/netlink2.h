#ifndef _JOOL_USR_NETLINK_H_
#define _JOOL_USR_NETLINK_H_

#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include "../common/genetlink.h"
#include "nat64/common/config.h"


/*
 * Assert we're compiling with libnl version >= 3.0
 *
 * Note: it looks like this shouldn't be here, since it's the configure script's responsibility.
 * However, the configure script seems to fail to detect this properly on RedHat (and maybe
 * others).
 */
#if !defined(LIBNL_VER_NUM)
	#error "Missing LIBNL dependency (need at least version 3)."
#endif
#if LIBNL_VER_NUM < LIBNL_VER(3,0)
	#error "Unsupported LIBNL library version number (< 3.0)."
#endif

int netlink_request(void *request, __u16 request_len, int (*cb)(struct nl_msg *, void *),
		void *cb_arg);


int netlink_request_multipart_done(void);

int netlink_init_multipart_connection(int (*cb)(struct nl_msg *, void *),void *cb_arg);

int netlink_request_multipart(void *request, __u16 request_len,	enum config_mode mode, enum config_operation operation);

void netlink_request_multipart_close(void);

int netlink_get_data(struct nl_msg *msg,enum config_mode command, __u8 ** data);

int genetlink_send_msg(struct request_hdr *usr_hdr, void *request,
		size_t request_len, int (*cb)(struct nl_msg *, void *), void *cb_arg);


#endif /* _JOOL_USR_NETLINK_H_ */