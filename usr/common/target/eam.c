#include "nat64/usr/eam.h"

#include <errno.h>
#include "nat64/common/config.h"
#include "nat64/common/str_utils.h"
#include "nat64/common/types.h"
#include "nat64/usr/netlink.h"


#define HDR_LEN sizeof(struct request_hdr)
#define PAYLOAD_LEN sizeof(union request_eamt)

struct display_args {
	display_flags flags;
	unsigned int row_count;
	union request_eamt *request;
};

static void print_eamt_entry(struct eamt_entry *entry, char *separator)
{
	char ipv6_str[INET6_ADDRSTRLEN];
	char *ipv4_str;

	inet_ntop(AF_INET6, &entry->prefix6.address, ipv6_str, sizeof(ipv6_str));
	ipv4_str = inet_ntoa(entry->prefix4.address);
	printf("%s/%u", ipv6_str, entry->prefix6.len);
	printf("%s", separator);
	printf("%s/%u", ipv4_str, entry->prefix4.len);
	printf("\n");
}

static int eam_display_response(struct jool_response *response, void *arg)
{
	struct eamt_entry *entries = response->payload;
	struct display_args *args = arg;
	__u16 entry_count, i;

	entry_count = response->payload_len / sizeof(*entries);

	if (args->flags & DF_CSV_FORMAT) {
		for (i = 0; i < entry_count; i++)
			print_eamt_entry(&entries[i], ",");
	} else {
		for (i = 0; i < entry_count; i++)
			print_eamt_entry(&entries[i], " - ");
	}

	args->row_count += entry_count;
	args->request->display.prefix4_set = response->hdr->pending_data;
	if (entry_count > 0) {
		struct eamt_entry *last = &entries[entry_count - 1];
		args->request->display.prefix4 = last->prefix4;
	}
	return 0;
}

int eam_display(char *iname, display_flags flags)
{
	unsigned char request[HDR_LEN + PAYLOAD_LEN];
	struct request_hdr *hdr = (struct request_hdr *)request;
	union request_eamt *payload = (union request_eamt *)(request + HDR_LEN);
	struct display_args args;
	int error;

	init_request_hdr(hdr, MODE_EAMT, OP_DISPLAY);
	payload->display.prefix4_set = false;
	memset(&payload->display.prefix4, 0, sizeof(payload->display.prefix4));
	args.flags = flags;
	args.row_count = 0;
	args.request = payload;

	if ((flags & DF_SHOW_HEADERS) && (flags & DF_CSV_FORMAT))
		printf("IPv6 Prefix,IPv4 Prefix\n");

	do {
		error = netlink_request(iname, request, sizeof(request),
				eam_display_response, &args);
		if (error)
			return error;
	} while (payload->display.prefix4_set);

	if (show_footer(flags)) {
		if (args.row_count > 0)
			log_info("  (Fetched %u entries.)", args.row_count);
		else
			log_info("  (empty)");
	}

	return 0;
}

static int eam_count_response(struct jool_response *response, void *arg)
{
	if (response->payload_len != sizeof(__u64)) {
		log_err("Jool's response is not the expected integer.");
		return -EINVAL;
	}

	printf("%llu\n", *((__u64 *)response->payload));
	return 0;
}

int eam_count(char *iname)
{
	unsigned char request[HDR_LEN + PAYLOAD_LEN];
	struct request_hdr *hdr = (struct request_hdr *)request;
	union request_eamt *payload = (union request_eamt *)(request + HDR_LEN);

	init_request_hdr(hdr, MODE_EAMT, OP_COUNT);
	memset(payload, 0, sizeof(*payload));

	return netlink_request(iname, &request, sizeof(request),
			eam_count_response, NULL);
}

int eam_add(char *iname, struct ipv6_prefix *prefix6,
		struct ipv4_prefix *prefix4, bool force)
{
	unsigned char request[HDR_LEN + PAYLOAD_LEN];
	struct request_hdr *hdr = (struct request_hdr *)request;
	union request_eamt *payload = (union request_eamt *)(request + HDR_LEN);

	init_request_hdr(hdr, MODE_EAMT, OP_ADD);
	payload->add.prefix4 = *prefix4;
	payload->add.prefix6 = *prefix6;
	payload->add.force = force;

	return netlink_request(iname, request, sizeof(request), NULL, NULL);
}

int eam_remove(char *iname, struct ipv6_prefix *prefix6,
		struct ipv4_prefix *prefix4)
{
	unsigned char request[HDR_LEN + PAYLOAD_LEN];
	struct request_hdr *hdr = (struct request_hdr *)request;
	union request_eamt *payload = (union request_eamt *)(request + HDR_LEN);

	init_request_hdr(hdr, MODE_EAMT, OP_REMOVE);
	if (prefix6) {
		payload->rm.prefix6_set = true;
		memcpy(&payload->rm.prefix6, prefix6, sizeof(*prefix6));
	} else {
		payload->rm.prefix6_set = false;
		memset(&payload->rm.prefix6, 0, sizeof(payload->rm.prefix6));
	}
	if (prefix4) {
		payload->rm.prefix4_set = true;
		memcpy(&payload->rm.prefix4, prefix4, sizeof(*prefix4));
	} else {
		payload->rm.prefix4_set = false;
		memset(&payload->rm.prefix4, 0, sizeof(payload->rm.prefix4));
	}

	return netlink_request(iname, request, sizeof(request), NULL, NULL);
}

int eam_flush(char *iname)
{
	unsigned char request[HDR_LEN + PAYLOAD_LEN];
	struct request_hdr *hdr = (struct request_hdr *)request;
	union request_eamt *payload = (union request_eamt *)(request + HDR_LEN);

	init_request_hdr(hdr, MODE_EAMT, OP_FLUSH);
	memset(payload, 0, sizeof(*payload));

	return netlink_request(iname, &request, sizeof(request), NULL, NULL);
}
