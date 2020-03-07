#ifndef SRC_COMMON_STATS_H_
#define SRC_COMMON_STATS_H_

/*
 * TODO (warning) Caller review needed.
 * Make sure there's a counter for every worthwhile event,
 * and also check the ICMP errors that sometimes need to be appended to them.
 */

/**
 * NOTE THAT ANY MODIFICATIONS MADE TO THIS STRUCTURE NEED TO BE CASCADED TO
 * jstat_metadatas.
 */
enum jool_stat_id {
	JSTAT_RECEIVED6 = 1,
	JSTAT_RECEIVED4,
	JSTAT_SUCCESS,

	JSTAT_BIB_ENTRIES,
	JSTAT_SESSIONS,

	JSTAT_ENOMEM,

	JSTAT_XLATOR_DISABLED,
	JSTAT_POOL6_UNSET,

	JSTAT_SKB_SHARED,
	JSTAT_L3HDR_OFFSET,
	JSTAT_SKB_TRUNCATED,
	JSTAT_FRAGMENTED_PING,
	JSTAT_HDR6,
	JSTAT_HDR4,

	JSTAT_UNKNOWN_L4_PROTO,
	JSTAT_UNKNOWN_ICMP6_TYPE,
	JSTAT_UNKNOWN_ICMP4_TYPE,
	JSTAT_DOUBLE_ICMP6_ERROR,
	JSTAT_DOUBLE_ICMP4_ERROR,
	JSTAT_UNKNOWN_PROTO_INNER,

	JSTAT_HAIRPIN_LOOP,
	JSTAT_POOL6_MISMATCH,
	JSTAT_POOL4_MISMATCH,
	JSTAT_ICMP6_FILTER,
	JSTAT_UNTRANSLATABLE_DST6,
	JSTAT_UNTRANSLATABLE_DST4,
	JSTAT_MASK_DOMAIN_NOT_FOUND,
	JSTAT_BIB6_NOT_FOUND,
	JSTAT_BIB4_NOT_FOUND,
	JSTAT_SESSION_NOT_FOUND,
	JSTAT_ADF,
	JSTAT_V4_SYN,
	JSTAT_SYN6_EXPECTED,
	JSTAT_SYN4_EXPECTED,

	JSTAT_TYPE1PKT,
	JSTAT_TYPE2PKT,
	JSTAT_SO_EXISTS,
	JSTAT_SO_FULL,

	JSTAT64_SRC,
	JSTAT64_DST,
	JSTAT64_PSKB_COPY,
	JSTAT64_ICMP_CSUM,
	JSTAT64_UNTRANSLATABLE_DEST_UNREACH,
	JSTAT64_UNTRANSLATABLE_PARAM_PROB,
	JSTAT64_UNTRANSLATABLE_PARAM_PROB_PTR,
	JSTAT64_TTL,
	JSTAT64_FRAG_THEN_EXT,
	JSTAT64_SEGMENTS_LEFT,

	JSTAT46_SRC,
	JSTAT46_DST,
	JSTAT46_PSKB_COPY,
	JSTAT46_ICMP_CSUM,
	JSTAT46_UNTRANSLATABLE_DEST_UNREACH,
	JSTAT46_UNTRANSLATABLE_PARAM_PROB,
	JSTAT46_UNTRANSLATABLE_PARAM_PROBLEM_PTR,
	JSTAT46_TTL,
	JSTAT46_SRC_ROUTE,
	JSTAT46_FRAGMENTED_ZERO_CSUM,

	JSTAT_FAILED_ROUTES,
	JSTAT_PKT_TOO_BIG,
	JSTAT_DST_OUTPUT,

	JSTAT_ICMP6ERR_SUCCESS,
	JSTAT_ICMP6ERR_FAILURE,
	JSTAT_ICMP4ERR_SUCCESS,
	JSTAT_ICMP4ERR_FAILURE,

	/* These 3 need to be last, and in this order. */
	JSTAT_UNKNOWN, /* "WTF was that" errors only. */
	JSTAT_PADDING,
	JSTAT_COUNT,
#define JSTAT_MAX (JSTAT_COUNT - 1)
};

#endif /* SRC_COMMON_STATS_H_ */
