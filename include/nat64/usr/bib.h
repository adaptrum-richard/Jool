#ifndef _JOOL_USR_BIB_H
#define _JOOL_USR_BIB_H

#include "nat64/common/types.h"
#include "nat64/usr/types.h"


int bib_display(char *iname, display_flags flags);
int bib_count(char *iname, display_flags flags);

int bib_add(char *iname, display_flags flags,
		struct ipv6_transport_addr *ipv6,
		struct ipv4_transport_addr *ipv4);
int bib_remove(char *iname, display_flags flags,
		struct ipv6_transport_addr *addr6,
		struct ipv4_transport_addr *addr4);


#endif /* _JOOL_USR_BIB_H */
