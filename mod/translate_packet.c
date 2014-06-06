/* TODO (warning) read the erratas more (6145 and 6146). */

#include "nat64/mod/translate_packet.h"
#include "nat64/comm/types.h"
#include "nat64/comm/constants.h"
#include "nat64/mod/random.h"
#include "nat64/mod/config.h"
#include "nat64/mod/ipv6_hdr_iterator.h"
#include "nat64/mod/send_packet.h"
#include "nat64/mod/icmp_wrapper.h"

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/list.h>
#include <linux/sort.h>
#include <linux/icmpv6.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/icmp.h>
#include <net/tcp.h>


/**
 * A bunch of pieces, that will eventually be merged into a sk_buff.
 * We also use it to describe incoming skbs, so we don't have to turn ICMP payloads into skbs when
 * we're translating error messages (since a pkt_parts in the stack is easier on the kernel than
 * a massive temporal sk_buff in dynamic memory, I think).
 */
struct pkt_parts {
	struct {
		l3_protocol proto;
		int len;
		void *ptr;
	} l3_hdr;
	struct {
		l4_protocol proto;
		int len;
		void *ptr;
	} l4_hdr;
	struct {
		int len;
		void *ptr;
	} payload;

	/**
	 * If this parts represents a incoming packet:
	 * - If skb is not NULL, it is the sk_buff these parts were computed from.
	 * - If skb is NULL, it's because there was no sk_buff in the first place (ie. it was generated
	 * from a packet contained inside a packet).
	 *
	 * If this parts represents a outgoing packet, then the result of joining the above parts is
	 * placed here.
	 */
	struct sk_buff *skb;
};

struct translation_steps {
	int (*skb_create_fn)(struct pkt_parts *in, struct sk_buff **out);
	/**
	 * The function that will translate the layer-3 header.
	 * Its purpose is to set the variables "out->l3_hdr.*", based on the packet described by "in".
	 */
	int (*l3_hdr_fn)(struct tuple *tuple, struct pkt_parts *in, struct pkt_parts *out);
	/**
	 * The function that will translate the layer-4 header and the payload.
	 * Layer 4 and payload are combined in a single function due to their strong interdependence.
	 * Its purpose is to set the variables "out->l4_hdr.*" and "out->payload.*", based on the
	 * packet described by "in".
	 */
	int (*l3_payload_fn)(struct tuple *, struct pkt_parts *in, struct pkt_parts *out);
	/**
	 * Post-processing involving the layer 3 header.
	 * Currently, this function fixes the header's lengths and checksum, which cannot be done in
	 * the functions above given that they generally require the rest of the packet to be known.
	 * Not all lengths and checksums might have that requirement, but just to be consistent do it
	 * always here, please.
	 * This function isn't run for inner packets. This is because inner packets are usually
	 * truncated, so updating checksums and lengths leads to values just as incorrect as the
	 * original ones.
	 * Also, this step happens after "out->skb" has been assembled, so it can dereference it.
	 */
	int (*l3_post_fn)(struct pkt_parts *out);
	/**
	 * This needs to happen before l4_post_fn(), because that one might deal with MTUs, which
	 * depend on skb's routing.
	 */
	int (*route_fn)(struct sk_buff *skb);
	/**
	 * Post-processing involving the layer 4 header. See l3_post_fn.
	 */
	int (*l4_post_fn)(struct tuple *tuple, struct pkt_parts *in, struct pkt_parts *out);
};

static struct translate_config *config;
static struct translation_steps steps[L3_PROTO_COUNT][L4_PROTO_COUNT];


/**
 * This function only makes sense if parts is an incoming packet.
 */
static inline bool is_inner_pkt(struct pkt_parts *parts)
{
	return parts->skb == NULL;
}

static int skb_to_parts(struct sk_buff *skb, struct pkt_parts *parts)
{
	parts->l3_hdr.proto = skb_l3_proto(skb);
	parts->l3_hdr.len = skb_l3hdr_len(skb);
	parts->l3_hdr.ptr = skb_network_header(skb);
	parts->l4_hdr.proto = skb_l4_proto(skb);
	parts->l4_hdr.len = skb_l4hdr_len(skb);
	parts->l4_hdr.ptr = skb_transport_header(skb);
	parts->payload.len = skb_payload_len(skb);
	parts->payload.ptr = skb_payload(skb);
	parts->skb = skb;

	return 0;
}

static int create_tcp_hdr(struct tuple *tuple, struct pkt_parts *in, struct pkt_parts *out)
{
	struct tcphdr *tcp_in = in->l4_hdr.ptr;
	struct tcphdr *tcp_out = out->l4_hdr.ptr;

	memcpy(tcp_out, tcp_in, sizeof(*tcp_in));
	tcp_out->source = cpu_to_be16(tuple->src.l4_id);
	tcp_out->dest = cpu_to_be16(tuple->dst.l4_id);

	memcpy(out->payload.ptr, in->payload.ptr, in->payload.len);

	return 0;
}

static int create_udp_hdr(struct tuple *tuple, struct pkt_parts *in, struct pkt_parts *out)
{
	struct udphdr *udp_in = in->l4_hdr.ptr;
	struct udphdr *udp_out = out->l4_hdr.ptr;

	memcpy(udp_out, udp_in, sizeof(*udp_in));
	udp_out->source = cpu_to_be16(tuple->src.l4_id);
	udp_out->dest = cpu_to_be16(tuple->dst.l4_id);

	memcpy(out->payload.ptr, in->payload.ptr, in->payload.len);

	return 0;
}

int translate_inner_packet(struct tuple *tuple, struct pkt_parts *in_inner,
		struct pkt_parts *out_outer)
{
	struct pkt_parts out_inner;
	struct tuple inner_tuple;
	struct translation_steps *current_steps;
	int error;

	inner_tuple.src = tuple->dst;
	inner_tuple.dst = tuple->src;
	inner_tuple.l3_proto = tuple->l3_proto;
	inner_tuple.l4_proto = tuple->l4_proto;

	out_inner.l3_hdr.proto = out_outer->l3_hdr.proto;
	out_inner.l3_hdr.len = out_outer->payload.len - in_inner->l4_hdr.len - in_inner->payload.len;
	out_inner.l3_hdr.ptr = out_outer->payload.ptr;
	out_inner.l4_hdr.proto = in_inner->l4_hdr.proto;
	out_inner.l4_hdr.len = in_inner->l4_hdr.len;
	out_inner.l4_hdr.ptr = out_inner.l3_hdr.ptr + out_inner.l3_hdr.len;
	out_inner.payload.len = in_inner->payload.len;
	out_inner.payload.ptr = out_inner.l4_hdr.ptr + out_inner.l4_hdr.len;

	current_steps = &steps[in_inner->l3_hdr.proto][in_inner->l4_hdr.proto];

	error = current_steps->l3_hdr_fn(&inner_tuple, in_inner, &out_inner);
	if (error)
		return error;
	error = current_steps->l3_payload_fn(&inner_tuple, in_inner, &out_inner);
	if (error)
		return error;

	return 0;
}

#include "translate_packet_6to4.c"
#include "translate_packet_4to6.c"

int translate_packet_init(void)
{
	__u16 default_plateaus[] = TRAN_DEF_MTU_PLATEAUS;

	config = kmalloc(sizeof(*config), GFP_ATOMIC);
	if (!config)
		return -ENOMEM;

	config->reset_traffic_class = TRAN_DEF_RESET_TRAFFIC_CLASS;
	config->reset_tos = TRAN_DEF_RESET_TOS;
	config->new_tos = TRAN_DEF_NEW_TOS;
	config->df_always_on = TRAN_DEF_DF_ALWAYS_ON;
	config->build_ipv4_id = TRAN_DEF_BUILD_IPV4_ID;
	config->lower_mtu_fail = TRAN_DEF_LOWER_MTU_FAIL;
	config->mtu_plateau_count = ARRAY_SIZE(default_plateaus);
	config->mtu_plateaus = kmalloc(sizeof(default_plateaus), GFP_ATOMIC);
	if (!config->mtu_plateaus) {
		log_err(ERR_ALLOC_FAILED, "Could not allocate memory to store the MTU plateaus.");
		kfree(config);
		return -ENOMEM;
	}
	config->min_ipv6_mtu = TRAN_DEF_MIN_IPV6_MTU;
	memcpy(config->mtu_plateaus, &default_plateaus, sizeof(default_plateaus));

	steps[L3PROTO_IPV6][L4PROTO_TCP].skb_create_fn = ttp64_create_out_skb;
	steps[L3PROTO_IPV6][L4PROTO_TCP].l3_hdr_fn = create_ipv4_hdr;
	steps[L3PROTO_IPV6][L4PROTO_TCP].l3_payload_fn = create_tcp_hdr;
	steps[L3PROTO_IPV6][L4PROTO_TCP].l3_post_fn = post_ipv4;
	steps[L3PROTO_IPV6][L4PROTO_TCP].l4_post_fn = post_tcp_ipv4;
	steps[L3PROTO_IPV6][L4PROTO_TCP].route_fn = route_ipv4;

	steps[L3PROTO_IPV6][L4PROTO_UDP].skb_create_fn = ttp64_create_out_skb;
	steps[L3PROTO_IPV6][L4PROTO_UDP].l3_hdr_fn = create_ipv4_hdr;
	steps[L3PROTO_IPV6][L4PROTO_UDP].l3_payload_fn = create_udp_hdr;
	steps[L3PROTO_IPV6][L4PROTO_UDP].l3_post_fn = post_ipv4;
	steps[L3PROTO_IPV6][L4PROTO_UDP].l4_post_fn = post_udp_ipv4;
	steps[L3PROTO_IPV6][L4PROTO_UDP].route_fn = route_ipv4;

	steps[L3PROTO_IPV6][L4PROTO_ICMP].skb_create_fn = ttp64_create_out_skb;
	steps[L3PROTO_IPV6][L4PROTO_ICMP].l3_hdr_fn = create_ipv4_hdr;
	steps[L3PROTO_IPV6][L4PROTO_ICMP].l3_payload_fn = create_icmp4_hdr_and_payload;
	steps[L3PROTO_IPV6][L4PROTO_ICMP].l3_post_fn = post_ipv4;
	steps[L3PROTO_IPV6][L4PROTO_ICMP].l4_post_fn = post_icmp4;
	steps[L3PROTO_IPV6][L4PROTO_ICMP].route_fn = route_ipv4;

	steps[L3PROTO_IPV4][L4PROTO_TCP].skb_create_fn = ttp46_create_out_skb;
	steps[L3PROTO_IPV4][L4PROTO_TCP].l3_hdr_fn = create_ipv6_hdr;
	steps[L3PROTO_IPV4][L4PROTO_TCP].l3_payload_fn = create_tcp_hdr;
	steps[L3PROTO_IPV4][L4PROTO_TCP].l3_post_fn = post_ipv6;
	steps[L3PROTO_IPV4][L4PROTO_TCP].l4_post_fn = post_tcp_ipv6;
	steps[L3PROTO_IPV4][L4PROTO_TCP].route_fn = route_ipv6;

	steps[L3PROTO_IPV4][L4PROTO_UDP].skb_create_fn = ttp46_create_out_skb;
	steps[L3PROTO_IPV4][L4PROTO_UDP].l3_hdr_fn = create_ipv6_hdr;
	steps[L3PROTO_IPV4][L4PROTO_UDP].l3_payload_fn = create_udp_hdr;
	steps[L3PROTO_IPV4][L4PROTO_UDP].l3_post_fn = post_ipv6;
	steps[L3PROTO_IPV4][L4PROTO_UDP].l4_post_fn = post_udp_ipv6;
	steps[L3PROTO_IPV4][L4PROTO_UDP].route_fn = route_ipv6;

	steps[L3PROTO_IPV4][L4PROTO_ICMP].skb_create_fn = ttp46_create_out_skb;
	steps[L3PROTO_IPV4][L4PROTO_ICMP].l3_hdr_fn = create_ipv6_hdr;
	steps[L3PROTO_IPV4][L4PROTO_ICMP].l3_payload_fn = create_icmp6_hdr_and_payload;
	steps[L3PROTO_IPV4][L4PROTO_ICMP].l3_post_fn = post_ipv6;
	steps[L3PROTO_IPV4][L4PROTO_ICMP].l4_post_fn = post_icmp6;
	steps[L3PROTO_IPV4][L4PROTO_ICMP].route_fn = route_ipv6;

	return 0;
}

void translate_packet_destroy(void)
{
	kfree(config->mtu_plateaus);
	kfree(config);
}

int clone_translate_config(struct translate_config *clone)
{
	struct translate_config *config_ref;
	__u16 plateaus_len;

	rcu_read_lock_bh();
	config_ref = rcu_dereference_bh(config);

	*clone = *config_ref;

	plateaus_len = clone->mtu_plateau_count * sizeof(*clone->mtu_plateaus);
	clone->mtu_plateaus = kmalloc(plateaus_len, GFP_ATOMIC);
	if (!clone->mtu_plateaus) {
		rcu_read_unlock_bh();
		log_err(ERR_ALLOC_FAILED, "Could not allocate a clone of the config's plateaus list.");
		return -ENOMEM;
	}
	memcpy(clone->mtu_plateaus, config_ref->mtu_plateaus, plateaus_len);

	rcu_read_unlock_bh();
	return 0;
}

static int be16_compare(const void *a, const void *b)
{
	return *(__u16 *)b - *(__u16 *)a;
}

static void be16_swap(void *a, void *b, int size)
{
	__u16 t = *(__u16 *)a;
	*(__u16 *)a = *(__u16 *)b;
	*(__u16 *)b = t;
}

int set_translate_config(__u32 operation, struct translate_config *new_config)
{
	struct translate_config *tmp_config;
	struct translate_config *old_config;

	/* Validate. */
	if (operation & MTU_PLATEAUS_MASK) {
		int i, j;

		if (new_config->mtu_plateau_count == 0) {
			log_err(ERR_MTU_LIST_EMPTY, "The MTU list received from userspace is empty.");
			return -EINVAL;
		}

		/* Sort descending. */
		sort(new_config->mtu_plateaus, new_config->mtu_plateau_count,
				sizeof(*new_config->mtu_plateaus), be16_compare, be16_swap);

		/* Remove zeroes and duplicates. */
		for (i = 0, j = 1; j < new_config->mtu_plateau_count; j++) {
			if (new_config->mtu_plateaus[j] == 0)
				break;
			if (new_config->mtu_plateaus[i] != new_config->mtu_plateaus[j]) {
				i++;
				new_config->mtu_plateaus[i] = new_config->mtu_plateaus[j];
			}
		}

		if (new_config->mtu_plateaus[0] == 0) {
			log_err(ERR_MTU_LIST_ZEROES, "The MTU list contains nothing but zeroes.");
			return -EINVAL;
		}

		new_config->mtu_plateau_count = i + 1;
	}

	/* Update. */
	tmp_config = kmalloc(sizeof(*tmp_config), GFP_KERNEL);
	if (!tmp_config)
		return -ENOMEM;

	old_config = config;
	*tmp_config = *old_config;

	if (operation & RESET_TCLASS_MASK)
		tmp_config->reset_traffic_class = new_config->reset_traffic_class;
	if (operation & RESET_TOS_MASK)
		tmp_config->reset_tos = new_config->reset_tos;
	if (operation & NEW_TOS_MASK)
		tmp_config->new_tos = new_config->new_tos;
	if (operation & DF_ALWAYS_ON_MASK)
		tmp_config->df_always_on = new_config->df_always_on;
	if (operation & BUILD_IPV4_ID_MASK)
		tmp_config->build_ipv4_id = new_config->build_ipv4_id;
	if (operation & LOWER_MTU_FAIL_MASK)
		tmp_config->lower_mtu_fail = new_config->lower_mtu_fail;

	if (operation & MTU_PLATEAUS_MASK) {
		__u16 new_mtus_len = new_config->mtu_plateau_count * sizeof(*new_config->mtu_plateaus);

		tmp_config->mtu_plateaus = kmalloc(new_mtus_len, GFP_ATOMIC);
		if (!tmp_config->mtu_plateaus) {
			log_err(ERR_ALLOC_FAILED, "Could not allocate the kernel's MTU plateaus list.");
			kfree(tmp_config);
			return -ENOMEM;
		}

		tmp_config->mtu_plateau_count = new_config->mtu_plateau_count;
		memcpy(tmp_config->mtu_plateaus, new_config->mtu_plateaus, new_mtus_len);
	}

	if (operation & MIN_IPV6_MTU_MASK)
		tmp_config->min_ipv6_mtu = new_config->min_ipv6_mtu;

	rcu_assign_pointer(config, tmp_config);
	synchronize_rcu_bh();

	if (old_config->mtu_plateaus != tmp_config->mtu_plateaus)
		kfree(old_config->mtu_plateaus);
	kfree(old_config);

	return 0;
}

static void set_frag_headers(struct ipv6hdr *hdr6_old, struct ipv6hdr *hdr6_new,
		u16 packet_size, u16 offset, bool mf)
{
	struct frag_hdr *hdrfrag_old = (struct frag_hdr *) (hdr6_old + 1);
	struct frag_hdr *hdrfrag_new = (struct frag_hdr *) (hdr6_new + 1);

	if (hdr6_new != hdr6_old)
		memcpy(hdr6_new, hdr6_old, sizeof(*hdr6_new));
	hdr6_new->payload_len = cpu_to_be16(packet_size - sizeof(*hdr6_new));

	hdrfrag_new->nexthdr = hdrfrag_old->nexthdr;
	hdrfrag_new->reserved = 0;
	hdrfrag_new->frag_off = build_ipv6_frag_off_field(offset, mf);
	hdrfrag_new->identification = hdrfrag_old->identification;
}

/**
 * Fragments "frag" until all the pieces are at most "min_ipv6_mtu" bytes long.
 * "min_ipv6_mtu" comes from the user's configuration.
 * The resulting smaller fragments are appended to frag's list (frag->next).
 *
 * Assumes frag has a fragment header.
 * Also assumes the following fields from frag->skb are properly set: network_header, head, data
 * and tail.
 *
 * Sorry, this function is probably our most convoluted one, but everything in it is too
 * inter-related so I don't know how to fix it without creating thousand-argument functions.
 */
static int divide(struct sk_buff *skb, __u16 min_ipv6_mtu)
{
	unsigned char *current_p;
	struct sk_buff *new_skb;
	struct sk_buff *prev_skb;
	struct ipv6hdr *first_hdr6 = ipv6_hdr(skb);
	u16 hdrs_size;
	u16 payload_max_size;
	u16 original_fragment_offset;
	bool original_mf;

	/* Prepare the helper values. */
	min_ipv6_mtu &= 0xFFF8;

	hdrs_size = sizeof(struct ipv6hdr) + sizeof(struct frag_hdr);
	payload_max_size = min_ipv6_mtu - hdrs_size;

	{
		struct frag_hdr *frag_header = (struct frag_hdr *) (first_hdr6 + 1);

		original_fragment_offset = get_fragment_offset_ipv6(frag_header);
		original_mf = is_more_fragments_set_ipv6(frag_header);
	}

	set_frag_headers(first_hdr6, first_hdr6, min_ipv6_mtu, original_fragment_offset, true);
	prev_skb = skb;

	/* Copy frag's overweight to newly-created fragments.  */
	current_p = skb_network_header(skb) + min_ipv6_mtu;
	while (current_p < skb_tail_pointer(skb)) {
		bool is_last = (skb_tail_pointer(skb) - current_p <= payload_max_size);
		u16 actual_payload_size = is_last
					? (skb_tail_pointer(skb) - current_p)
					: (payload_max_size & 0xFFF8);
		u16 actual_total_size = hdrs_size + actual_payload_size;

		new_skb = alloc_skb(LL_MAX_HEADER /* kernel's reserved + layer 2. */
				+ actual_total_size, /* l3 header + l4 header + packet data. */
				GFP_ATOMIC);
		if (!new_skb)
			return -ENOMEM;

		skb_reserve(new_skb, LL_MAX_HEADER);
		skb_put(new_skb, actual_total_size);
		skb_reset_mac_header(new_skb);
		skb_reset_network_header(new_skb);
		skb_reset_transport_header(new_skb);
		new_skb->protocol = skb->protocol;
		new_skb->mark = skb->mark;
		skb_dst_set(new_skb, dst_clone(skb_dst(skb)));
		new_skb->dev = skb->dev;

		set_frag_headers(first_hdr6, ipv6_hdr(new_skb), actual_total_size,
				original_fragment_offset + (current_p - skb->data - hdrs_size),
				is_last ? original_mf : true);
		memcpy(skb_network_header(new_skb) + hdrs_size, current_p, actual_payload_size);

		skb_set_jcb(new_skb, L3PROTO_IPV6, L4PROTO_NONE, skb_network_header(new_skb) + hdrs_size,
				skb_original_skb(skb));

		prev_skb->next = new_skb;
		new_skb->prev = prev_skb;

		current_p += actual_payload_size;
		prev_skb = new_skb;
	}

	/* Finally truncate frag and we're done. */
	skb_put(skb, -(skb->len - min_ipv6_mtu));

	return 0;
}

static int fragment_if_too_big(struct sk_buff *skb_in, struct sk_buff *skb_out)
{
	__u16 min_ipv6_mtu;

	if (skb_l3_proto(skb_out) != L3PROTO_IPV6)
		return 0; /* IPv4 routers fragment dandily, so let them do it. */

	rcu_read_lock_bh();
	min_ipv6_mtu = rcu_dereference_bh(config)->min_ipv6_mtu;
	rcu_read_unlock_bh();

	if (skb_out->len <= min_ipv6_mtu)
		return 0; /* No need for fragmentation. */

	if (skb_l4_proto(skb_out) == L4PROTO_ICMP && is_icmp6_error(icmp6_hdr(skb_out)->icmp6_type)) {
		/* ICMP errors are supposed to be truncated, not fragmented. */
		skb_trim(skb_out, min_ipv6_mtu);
		return 0;
	}

	if (is_dont_fragment_set(ip_hdr(skb_in))) {
		/* We're not supposed to fragment; yay. */
		icmp64_send(skb_in, ICMPERR_FRAG_NEEDED, cpu_to_be32(min_ipv6_mtu - 20));
		log_info("Packet is too big (%u bytes; MTU: %u); dropping.", skb_out->len, min_ipv6_mtu);
		return -EINVAL;
	}

	return divide(skb_out, min_ipv6_mtu);
}

verdict translating_the_packet(struct tuple *tuple, struct sk_buff *in_skb,
		struct sk_buff **out_skb)
{
	struct pkt_parts in;
	struct pkt_parts out;
	struct translation_steps *current_steps = &steps[skb_l3_proto(in_skb)][skb_l4_proto(in_skb)];

	log_debug("Step 4: Translating the Packet");
	*out_skb = NULL;

	if (is_error(skb_to_parts(in_skb, &in)))
		goto fail;
	if (is_error(current_steps->skb_create_fn(&in, out_skb)))
		goto fail;
	if (is_error(skb_to_parts(*out_skb, &out)))
		goto fail;
	if (is_error(current_steps->l3_hdr_fn(tuple, &in, &out)))
		goto fail;
	if (is_error(current_steps->l3_payload_fn(tuple, &in, &out)))
		goto fail;
	if (is_error(current_steps->l3_post_fn(&out)))
		goto fail;
	if (is_error(current_steps->route_fn(out.skb)))
		goto fail;
	if (is_error(current_steps->l4_post_fn(tuple, &in, &out)))
		goto fail;

	if (is_error(fragment_if_too_big(in_skb, out.skb)))
		goto fail;

	log_debug("Done step 4.");
	return VER_CONTINUE;

fail:
	kfree_skb_queued(*out_skb);
	*out_skb = NULL;
	return VER_DROP;
}
