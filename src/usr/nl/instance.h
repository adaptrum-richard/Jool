#ifndef SRC_USR_NL_INSTANCE_H_
#define SRC_USR_NL_INSTANCE_H_

#include "common/config.h"
#include "usr/nl/jool_socket.h"

typedef struct jool_result (*instance_foreach_cb)(
		struct instance_entry_usr *instance, void *arg);

struct jool_result instance_foreach(struct jool_socket *sk,
		instance_foreach_cb cb, void *args);
struct jool_result instance_hello(struct jool_socket *sk, char *iname,
		enum instance_hello_status *status);
struct jool_result instance_add(struct jool_socket *sk, xlator_framework xf,
		char *iname, struct ipv6_prefix *pool6);
struct jool_result instance_rm(struct jool_socket *sk, char *iname);
struct jool_result instance_flush(struct jool_socket *sk);

#endif /* SRC_USR_NL_INSTANCE_H_ */
