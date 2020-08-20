#include <linux/module.h>
#include <linux/printk.h>

#include "framework/unit_test.h"
#include "framework/skb_generator.h"

#include "mod/common/mapt.h"
#include "mod/common/packet.h"
#include "mod/common/db/fmr.h"
#include "mod/common/db/global.h"

MODULE_LICENSE(JOOL_LICENSE);
MODULE_AUTHOR("Alberto Leiva");
MODULE_DESCRIPTION("MAP-T address translation module test.");

static struct xlator br;
static struct xlator ce;

verdict rule_xlat46(struct xlation *state, struct mapping_rule *rule,
		__be32 in, unsigned int port,
		struct in6_addr *out);

static int setup_mapt(void)
{
	struct ipv6_prefix pool6;
	struct ipv6_prefix eui6p;
	struct mapping_rule bmr;
	int error;

	error = prefix6_parse("2001:db8:ffff::/64", &pool6);
	if (error)
		return error;

	error = xlator_init(&br, NULL, "BR", XT_MAPT | XF_IPTABLES, &pool6);
	if (error)
		return error;
	error = xlator_init(&ce, NULL, "CE", XT_MAPT | XF_IPTABLES, &pool6);
	if (error)
		return error;

	bmr.ea_bits_length = 16;
	error = prefix6_parse("2001:db8:12:3400::/56", &eui6p)
	    || prefix6_parse("2001:db8::/40", &bmr.prefix6)
	    || prefix4_parse("192.0.2.0/24", &bmr.prefix4)
	    || mapt_init(&br.globals.mapt, NULL, NULL, 6, 8)
	    || mapt_init(&ce.globals.mapt, &eui6p, &bmr, 6, 8);
	if (error)
		return error;

	return fmrt_add(br.mapt.fmrt, &bmr);
}

void teardown_mapt(void)
{
	xlator_put(&br);
	xlator_put(&ce);
}

/* RFC7599 Appendix A Example 2 */
static bool br46(void)
{
	struct xlation state;
	struct in6_addr src;
	struct in6_addr dst;
	bool success;

	xlation_init(&state, &br);
	if (!ASSERT_INT(0, create_skb4_tcp("10.2.3.4", 80, "192.0.2.18", 1232, 4, 64, &state.in.skb), "SKB creator"))
		return false;
	if (!ASSERT_INT(0, pkt_init_ipv4(&state, state.in.skb), "Pkt init"))
		return false;

	success  = ASSERT_INT(VERDICT_CONTINUE, translate_addrs46_mapt(&state, &src, &dst), "translate_addrs46_mapt()");
	success &= ASSERT_ADDR6("2001:db8:ffff:0:a:203:0400::", &src, "Result source");
	success &= ASSERT_ADDR6("2001:db8:12:3400::c000:212:34", &dst, "Result destination");

	kfree_skb(state.in.skb);
	return success;
}

static bool br64(void)
{
	struct xlation state;
	struct in_addr src;
	struct in_addr dst;
	bool success;

	xlation_init(&state, &br);
	if (!ASSERT_INT(0, create_skb6_tcp("2001:db8:12:3400::c000:212:34", 1232, "2001:db8:ffff:0:a:203:0400::", 80, 4, 64, &state.in.skb), "SKB creator"))
		return false;
	if (!ASSERT_INT(0, pkt_init_ipv6(&state, state.in.skb), "Pkt init"))
		return false;

	success  = ASSERT_INT(VERDICT_CONTINUE, translate_addrs64_mapt(&state, &src.s_addr, &dst.s_addr), "translate_addrs64_mapt()");
	success &= ASSERT_ADDR4("192.0.2.18", &src, "Result source");
	success &= ASSERT_ADDR4("10.2.3.4", &dst, "Result destination");

	kfree_skb(state.in.skb);
	return success;
}

/* RFC7599 Appendix A Example 3 */
static bool ce46(void)
{
	struct xlation state;
	struct in6_addr src;
	struct in6_addr dst;
	bool success;

	xlation_init(&state, &ce);
	if (!ASSERT_INT(0, create_skb4_tcp("192.0.2.18", 1232, "10.2.3.4", 80, 4, 64, &state.in.skb), "SKB creator"))
		return false;
	if (!ASSERT_VERDICT(CONTINUE, pkt_init_ipv4(&state, state.in.skb), "Pkt init"))
		return false;

	success  = ASSERT_VERDICT(CONTINUE, translate_addrs46_mapt(&state, &src, &dst), "translate_addrs46_mapt()");
	success &= ASSERT_ADDR6("2001:db8:12:3400::c000:212:34", &src, "Result source");
	success &= ASSERT_ADDR6("2001:db8:ffff:0:a:203:400::", &dst, "Result destination");

	kfree_skb(state.in.skb);
	return success;
}

static bool ce64(void)
{
	struct xlation state;
	struct in_addr src;
	struct in_addr dst;
	bool success;

	xlation_init(&state, &ce);
	if (!ASSERT_INT(0, create_skb6_tcp("2001:db8:ffff:0:a:203:400::", 80, "2001:db8:12:3400::c000:212:34", 1232, 4, 64, &state.in.skb), "SKB creator"))
		return false;
	if (!ASSERT_VERDICT(CONTINUE, pkt_init_ipv6(&state, state.in.skb), "Pkt init"))
		return false;

	success  = ASSERT_VERDICT(CONTINUE, translate_addrs64_mapt(&state, &src.s_addr, &dst.s_addr), "translate_addrs64_mapt()");
	success &= ASSERT_ADDR4("10.2.3.4", &src, "Result source");
	success &= ASSERT_ADDR4("192.0.2.18", &dst, "Result destination");

	kfree_skb(state.in.skb);
	return success;
}

static bool check_variant(unsigned int a, unsigned int k,
		unsigned int r, unsigned int o,
		char const *test, unsigned int port, char const *expected)
{
	struct xlation state;
	struct mapping_rule rule;
	struct in_addr addr4;
	struct in6_addr addr6;
	bool success;

	memset(&state, 0, sizeof(state));
	state.jool.globals.mapt.prpf.a = a;
	state.jool.globals.mapt.prpf.k = k;

	memset(&rule, 0, sizeof(rule));
	rule.prefix6.addr.s6_addr32[0] = cpu_to_be32(0x20010db8);
	rule.prefix6.len = 64 - o;
	rule.prefix4.addr.s_addr = cpu_to_be32(0xc0000200);
	rule.prefix4.len = r;
	rule.ea_bits_length = o;

	if (!ASSERT_INT(0, str_to_addr4(test, &addr4), "IPv4 Address")) {
		pr_err("'%s' does not parse as an IPv4 address.\n", test);
		return false;
	}

	pr_info("a:%u k:%u r:%u o:%u %s:%u\n", a, k, r, o, test, port);

	success = ASSERT_VERDICT(
		CONTINUE,
		rule_xlat46(&state, &rule, addr4.s_addr, port, &addr6),
		"Function verdict"
	);
	success &= ASSERT_ADDR6(expected, &addr6, "IPv6 Address");

	return success;
}

static bool a_plus_k_plus_m_equals_16(void)
{
	bool success = true;

	/*
	 * See rfc7597#section-5.2.
	 *
	 * When `a + k + m = 16`, each CE has *one* IPv4 address (`o + r = 32`)
	 * or a *portion* of one IPv4 address (`o + r > 32`). (If you don't
	 * understand why, it's because 16 is exactly the number of bits you
	 * need to represent *one* full range of ports.)
	 *
	 * When `o + r = 32`, `o = p` and therefore `q = 0`. (ie. the PSID is
	 * included in the EA-bits only if it contributes routing information.)
	 *
	 * When `o + r > 32`,
	 *
	 *	p = 32 - r (By RFC, section 5.2)
	 *	o = p + q (By definition)
	 *	Hence o + r = 32 + q
	 *
	 * This doesn't contradict our observations from `o + r = 32`, so we can
	 * generalize this as "if `a + k + m = 16`, then `o + r = 32 + q`."
	 *
	 * Presumably, `q = k`. (I still have yet to understand if there's a
	 * difference between the two.)
	 *
	 * So these tests are all designed around `o + r = 32 + k`.
	 */

	/*
	 * First, pivot around r = 24.
	 * (r = 24, try several combinations of a/k/m, use above equation to
	 * infer o.)
	 * (The commented column is m.)
	 */
	success &= check_variant( 0,  0, /* 16, */ 24,  8, "192.0.2.89", 1234, "2001:db8:0:59::c000:259:0");
	success &= check_variant( 0, 16, /*  0, */ 24, 24, "192.0.2.89", 1234, "2001:db8:59:4d2::c000:259:4d2");
	success &= check_variant(16,  0, /*  0, */ 24,  8, "192.0.2.89", 1234, "2001:db8:0:59:0:c000:259:0");
	success &= check_variant( 0,  8, /*  8, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:5904:0:c000:259:4");
	success &= check_variant( 8,  0, /*  8, */ 24,  8, "192.0.2.89", 1234, "2001:db8:0:59:0:c000:259:0");
	success &= check_variant( 8,  8, /*  0, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:59d2:0:c000:259:d2");
	success &= check_variant( 0,  7, /*  9, */ 24, 15, "192.0.2.89", 1234, "2001:db8:0:2c82:0:c000:259:2");
	success &= check_variant( 7,  0, /*  9, */ 24,  8, "192.0.2.89", 1234, "2001:db8:0:59:0:c000:259:0");
	success &= check_variant( 7,  9, /*  0, */ 24, 17, "192.0.2.89", 1234, "2001:db8:0:b2d2:0:c000:259:d2");
	success &= check_variant( 0,  9, /*  7, */ 24, 17, "192.0.2.89", 1234, "2001:db8:0:b209:0:c000:259:9");
	success &= check_variant( 9,  0, /*  7, */ 24,  8, "192.0.2.89", 1234, "2001:db8:0:59:0:c000:259:0");
	success &= check_variant( 9,  7, /*  0, */ 24, 15, "192.0.2.89", 1234, "2001:db8:0:2cd2:0:c000:259:52");
	success &= check_variant( 6,  8, /*  2, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:5934:0:c000:259:34");

	/*
	 * Now, pivot around o = 16.
	 * (o = 16, try several combinations of a/k/m, use above equation to
	 * infer r.)
	 */
	success &= check_variant( 0,  0, /* 16, */ 16, 16, "192.0.2.89", 1234, "2001:db8:0:259::c000:259:0");
	success &= check_variant( 0, 16, /*  0, */ 32, 16, "192.0.2.89", 1234, "2001:db8:0:4d2::c000:259:4d2");
	success &= check_variant(16,  0, /*  0, */ 16, 16, "192.0.2.89", 1234, "2001:db8:0:259::c000:259:0");
//	success &= check_variant( 0,  8, /*  8, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:____::c000:259:____");
	success &= check_variant( 8,  0, /*  8, */ 16, 16, "192.0.2.89", 1234, "2001:db8:0:259::c000:259:0");
//	success &= check_variant( 8,  8, /*  0, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:____::c000:259:____");
	success &= check_variant( 0,  7, /*  9, */ 23, 16, "192.0.2.89", 1234, "2001:db8:0:2c82::c000:259:2");
	success &= check_variant( 7,  0, /*  9, */ 16, 16, "192.0.2.89", 1234, "2001:db8:0:259::c000:259:0");
	success &= check_variant( 7,  9, /*  0, */ 25, 16, "192.0.2.89", 1234, "2001:db8:0:b2d2::c000:259:d2");
	success &= check_variant( 0,  9, /*  7, */ 25, 16, "192.0.2.89", 1234, "2001:db8:0:b209::c000:259:09");
	success &= check_variant( 9,  0, /*  7, */ 16, 16, "192.0.2.89", 1234, "2001:db8:0:259::c000:259:0");
	success &= check_variant( 9,  7, /*  0, */ 23, 16, "192.0.2.89", 1234, "2001:db8:0:2cd2::c000:259:52");
//	success &= check_variant( 6,  8, /*  2, */ 24, 16, "192.0.2.89", 1234, "2001:db8:0:____::c000:259:____");

	return success;
}

int init_module(void)
{
	struct test_group test = {
		.name = "MAP-T",
		.setup_fn = setup_mapt,
		.teardown_fn = teardown_mapt,
	};

	if (test_group_begin(&test))
		return -EINVAL;

	test_group_test(&test, br46, "BR address translation, 4->6");
	test_group_test(&test, br64, "BR address translation, 6->4");
	test_group_test(&test, ce46, "CE address translation, 4->6");
	test_group_test(&test, ce64, "CE address translation, 6->4");
	test_group_test(&test, a_plus_k_plus_m_equals_16, "a + k + m = 16");

	return test_group_end(&test);
}

void cleanup_module(void)
{
	/* No code. */
}
