/*
 * Copyright 2018 Viveris Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file   decomp_rfc5225_ip_udp.c
 * @brief  ROHC decompression context for the ROHCv2 IP/UDP profile
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 */

#include "rohc_decomp.h"
#include "rohc_decomp_internals.h"
#include "rohc_traces_internal.h"
#include "rohc_decomp_detect_packet.h" /* for rohc_decomp_packet_is_ir() */
#include "protocols/ip_numbers.h"
#include "protocols/ip.h"
#include "protocols/rfc5225.h"
#include "schemes/ip_ctxt.h"
#include "schemes/decomp_wlsb.h"
#include "schemes/decomp_crc.h"
#include "schemes/rfc4996.h"
#include "rohc_bit_ops.h"
#include "crc.h"
#include "rohc_debug.h"
#include "interval.h"

#include <string.h>


/** Define the ROHCv2 IP/UDP part of the decompression profile context */
struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt
{
	/** The LSB decoding context of MSN */
	struct rohc_lsb_decode msn_lsb_ctxt;

	/** The LSB decoding context of innermost IP-ID offset */
	struct rohc_lsb_decode ip_id_offset_lsb_ctxt;

	/** The reorder ratio that compressor sent to decompressor */
	rohc_reordering_offset_t reorder_ratio;

	size_t ip_contexts_nr;
	ip_context_t ip_contexts[ROHC_MAX_IP_HDRS];

	/** The UDP source port */
	uint16_t udp_sport;
	/** The UDP destination port */
	uint16_t udp_dport;
	/** Whether the UDP checksum is used or not */
	bool udp_checksum_used;

	uint16_t rtp_ssrc;
};


/** The outer or inner IP bits extracted from ROHC headers */
struct rohc_rfc5225_ip_bits
{
	uint8_t version:4;  /**< The version bits found in static chain of IR header */

	uint8_t tos_tc_bits;         /**< The IP TOS/TC bits */
	size_t tos_tc_bits_nr;       /**< The number of IP TOS/TC bits */

	uint8_t id_behavior:2;       /**< The IP-ID behavior bits */
	size_t id_behavior_nr;       /**< The number of IP-ID behavior bits */
	struct rohc_lsb_field16 id;  /**< The IP-ID bits */

	uint8_t df:1;    /**< The DF bits found in dynamic chain of IR/IR-DYN
	                      header or in extension header */
	size_t df_nr;    /**< The number of DF bits found */

	uint8_t ttl_hl;   /**< The IP TTL/HL bits */
	size_t ttl_hl_nr; /**< The number of IP TTL/HL bits */

	uint8_t proto;   /**< The protocol/next header bits found static chain
	                      of IR header or in extension header */
	size_t proto_nr; /**< The number of protocol/next header bits */

	uint32_t flowid:20;  /**< The IPv6 flow ID bits found in static chain */
	size_t flowid_nr;    /**< The number of flow label bits */

	uint8_t saddr[16];   /**< The source address bits found in static chain */
	size_t saddr_nr;     /**< The number of source address bits */

	uint8_t daddr[16];   /**< The destination address bits found in static chain */
	size_t daddr_nr;     /**< The number of source address bits */

	/* TODO: handle IPv6 extension headers */
};


/** The bits extracted from ROHCv2 IP/UDP header */
struct rohc_rfc5225_bits
{
	/** The extracted bits related to the IP headers */
	struct rohc_rfc5225_ip_bits ip[ROHC_MAX_IP_HDRS];
	size_t ip_nr;   /**< The number of parsed IP headers */

	/** The extracted bits of the Master Sequence Number (MSN) of the packet */
	struct rohc_lsb_field16 msn;

	rohc_reordering_offset_t reorder_ratio; /**< The reorder ratio bits */
	size_t reorder_ratio_nr; /**< The number of reorder ratio bits */

	uint8_t outer_ip_flag;   /**< The outer_ip_flag bits */
	size_t outer_ip_flag_nr; /**< The number of outer_ip_flag bits */

	struct rohc_decomp_crc ctrl_crc;

	uint16_t udp_sport;  /**< The UDP source port bits */
	size_t udp_sport_nr; /**< The number of UDP source port bits */
	uint16_t udp_dport;  /**< The UDP destination port bits */
	size_t udp_dport_nr; /**< The number of UDP destination port bits */
	uint16_t udp_checksum;  /**< The UDP checksum bits */
	size_t udp_checksum_nr; /**< The number of UDP checksum bits */
	
	uint16_t rtp_ssrc;
	size_t rtp_ssrc_nr;
};


/** The IP values decoded from the extracted ROHC bits */
struct rohc_rfc5225_decoded_ip
{
	uint8_t version:4;   /**< The decoded version field */
	uint8_t tos_tc;      /**< The decoded TOS/TC field */
	rohc_ip_id_behavior_t id_behavior; /**< The decoded IP-ID behavior (IPv4 only) */
	uint16_t id;         /**< The decoded IP-ID field (IPv4 only) */
	uint8_t df:1;        /**< The decoded DF field (IPv4 only) */
	uint8_t ttl;         /**< The decoded TTL/HL field */
	uint8_t proto;       /**< The decoded protocol/NH field */
	uint8_t nbo:1;       /**< The decoded NBO field (IPv4 only) */
	uint8_t rnd:1;       /**< The decoded RND field (IPv4 only) */
	uint32_t flowid:20;  /**< The decoded flow ID field (IPv6 only) */
	uint8_t saddr[16];   /**< The decoded source address field */
	uint8_t daddr[16];   /**< The decoded destination address field */
};


/** The values decoded from the bits extracted from ROHCv2 IP/UDP header */
struct rohc_rfc5225_decoded
{
	/** The decoded values related to the IP headers */
	struct rohc_rfc5225_decoded_ip ip[ROHC_MAX_IP_HDRS];
	size_t ip_nr;  /**< The number of the decoded IP headers */

	/** The Master Sequence Number (MSN) of the packet */
	uint16_t msn;

	rohc_reordering_offset_t reorder_ratio; /**< The reorder ratio decoded */

	uint16_t udp_sport; /**< The UDP source port decoded */
	uint16_t udp_dport; /**< The UDP destination port decoded */
	uint16_t udp_checksum; /**< The UDP checksum decoded */
	bool udp_checksum_used; /**< Whether the UDP checksum is used or not */

	uint16_t ssrc;
};


/*
 * Prototypes of private functions
 */

static bool decomp_rfc5225_ip_udp_rtp_new_context(const struct rohc_decomp_ctxt *const context,
                                              void **const persist_ctxt,
                                              struct rohc_decomp_volat_ctxt *const volat_ctxt)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));

static void decomp_rfc5225_ip_udp_rtp_free_context(struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt,
                                               const struct rohc_decomp_volat_ctxt *const volat_ctxt)
	__attribute__((nonnull(1, 2)));

static rohc_packet_t decomp_rfc5225_ip_udp_rtp_detect_pkt_type(const struct rohc_decomp_ctxt *const context,
                                                           const uint8_t *const rohc_packet,
                                                           const size_t rohc_length,
                                                           const size_t large_cid_len)
	__attribute__((warn_unused_result, nonnull(1, 2)));

static bool decomp_rfc5225_ip_udp_rtp_parse_pkt(const struct rohc_decomp_ctxt *const context,
                                            const struct rohc_buf rohc_packet,
                                            const size_t large_cid_len,
                                            rohc_packet_t *const packet_type,
                                            struct rohc_decomp_crc *const extr_crc,
                                            struct rohc_rfc5225_bits *const bits,
                                            size_t *const rohc_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 4, 5, 6, 7)));

static void decomp_rfc5225_ip_udp_rtp_reset_extr_bits(const struct rohc_decomp_ctxt *const ctxt,
                                                  struct rohc_rfc5225_bits *const bits)
	__attribute__((nonnull(1, 2)));

static bool decomp_rfc5225_ip_udp_rtp_parse_ir(const struct rohc_decomp_ctxt *const ctxt,
                                           const struct rohc_buf rohc_pkt,
                                           const size_t large_cid_len,
                                           struct rohc_decomp_crc *const extr_crc,
                                           struct rohc_rfc5225_bits *const bits,
                                           size_t *const rohc_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 4, 5, 6)));

static bool decomp_rfc5225_ip_udp_rtp_parse_co_repair(const struct rohc_decomp_ctxt *const ctxt,
                                                  const struct rohc_buf rohc_pkt,
                                                  const size_t large_cid_len,
                                                  struct rohc_decomp_crc *const hdr_crc,
                                                  struct rohc_rfc5225_bits *const bits,
                                                  size_t *const rohc_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 4, 5, 6)));

/* static chain */
static bool decomp_rfc5225_ip_udp_rtp_parse_static_chain(const struct rohc_decomp_ctxt *const ctxt,
                                                     const uint8_t *const rohc_pkt,
                                                     const size_t rohc_len,
                                                     struct rohc_rfc5225_bits *const bits,
                                                     size_t *const parsed_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5)));
static int decomp_rfc5225_ip_udp_rtp_parse_static_ip(const struct rohc_decomp_ctxt *const ctxt,
                                                 const uint8_t *const rohc_pkt,
                                                 const size_t rohc_len,
                                                 struct rohc_rfc5225_ip_bits *const ip_bits,
                                                 bool *const is_innermost)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5)));
static int decomp_rfc5225_ip_udp_rtp_parse_static_udp(const struct rohc_decomp_ctxt *const ctxt,
                                                  const uint8_t *rohc_pkt,
                                                  const size_t rohc_len,
                                                  struct rohc_rfc5225_bits *const bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));

/* dynamic chain */
static bool decomp_rfc5225_ip_udp_rtp_parse_dyn_chain(const struct rohc_decomp_ctxt *const ctxt,
                                                  const uint8_t *const rohc_pkt,
                                                  const size_t rohc_len,
                                                  struct rohc_rfc5225_bits *const bits,
                                                  size_t *const parsed_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5)));
static int decomp_rfc5225_ip_udp_rtp_parse_dyn_ip(const struct rohc_decomp_ctxt *const ctxt,
                                              const uint8_t *const rohc_pkt,
                                              const size_t rohc_len,
                                              struct rohc_rfc5225_ip_bits *const ip_bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));
static int decomp_rfc5225_ip_udp_rtp_parse_dyn_udp(const struct rohc_decomp_ctxt *const ctxt,
                                               const uint8_t *rohc_pkt,
                                               const size_t rohc_len,
                                               struct rohc_rfc5225_bits *const bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));

/* irregular chain */
/*static bool decomp_rfc5225_ip_udp_rtp_parse_irreg_chain(const struct rohc_decomp_ctxt *const ctxt,
                                                    const uint8_t *const rohc_pkt,
                                                    const size_t rohc_len,
                                                    const rohc_ip_id_behavior_t innermost_ip_id_behavior,
                                                    struct rohc_rfc5225_bits *const bits,
                                                    size_t *const parsed_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 5, 6)));
static int decomp_rfc5225_ip_udp_rtp_parse_irreg_ip(const struct rohc_decomp_ctxt *const ctxt,
                                                const uint8_t *const rohc_pkt,
                                                const size_t rohc_len,
                                                const bool is_innermost,
                                                const rohc_ip_id_behavior_t ip_id_behavior,
                                                const bool outer_ip_flag,
                                                struct rohc_rfc5225_ip_bits *const ip_bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 7)));
static int decomp_rfc5225_ip_udp_rtp_parse_irreg_udp(const struct rohc_decomp_ctxt *const ctxt,
                                                 const uint8_t *rohc_pkt,
                                                 const size_t rohc_len,
                                                 struct rohc_rfc5225_bits *const bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));
*/
/* decoding parsed fields */
static bool decomp_rfc5225_ip_udp_rtp_decode_bits(const struct rohc_decomp_ctxt *const ctxt,
                                              const struct rohc_rfc5225_bits *const bits,
                                              const size_t payload_len,
                                              struct rohc_rfc5225_decoded *const decoded)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));
static bool decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdrs(const struct rohc_decomp_ctxt *const ctxt,
                                                      const struct rohc_rfc5225_bits *const bits,
                                                      struct rohc_rfc5225_decoded *const decoded)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));
static bool decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdr(const struct rohc_decomp_ctxt *const context,
                                                     const struct rohc_rfc5225_ip_bits *const ip_bits,
                                                     const ip_context_t *const ip_ctxt,
                                                     const uint16_t decoded_msn,
                                                     struct rohc_rfc5225_decoded_ip *const ip_decoded)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));

/* building decompressed headers */
static rohc_status_t decomp_rfc5225_ip_udp_rtp_build_hdrs(const struct rohc_decomp *const decomp,
                                                      const struct rohc_decomp_ctxt *const context,
                                                      const rohc_packet_t packet_type,
                                                      const struct rohc_decomp_crc *const extr_crc,
                                                      const struct rohc_rfc5225_decoded *const decoded,
                                                      const size_t payload_len,
                                                      struct rohc_buf *const uncomp_hdrs,
                                                      size_t *const uncomp_hdrs_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5, 7, 8)));
static bool decomp_rfc5225_ip_udp_rtp_build_ip_hdrs(const struct rohc_decomp_ctxt *const context,
                                                const struct rohc_rfc5225_decoded *const decoded,
                                                struct rohc_buf *const uncomp_packet,
                                                size_t *const ip_hdrs_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4)));
static bool decomp_rfc5225_ip_udp_rtp_build_ip_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                               const struct rohc_rfc5225_decoded_ip *const decoded,
                                               struct rohc_buf *const uncomp_pkt,
                                               size_t *const ip_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4)));
static bool decomp_rfc5225_ip_udp_rtp_build_ipv4_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                 const struct rohc_rfc5225_decoded_ip *const decoded,
                                                 struct rohc_buf *const uncomp_pkt,
                                                 size_t *const ip_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4)));
static bool decomp_rfc5225_ip_udp_rtp_build_ipv6_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                 const struct rohc_rfc5225_decoded_ip *const decoded,
                                                 struct rohc_buf *const uncomp_pkt,
                                                 size_t *const ip_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4)));
static bool decomp_rfc5225_ip_udp_rtp_build_udp_hdr(const struct rohc_decomp_ctxt *const context,
                                                const struct rohc_rfc5225_decoded *const decoded,
                                                const size_t payload_len,
                                                struct rohc_buf *const uncomp_packet,
                                                size_t *const udp_hdr_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5)));

/* updating context */
static void decomp_rfc5225_ip_udp_rtp_update_ctxt(struct rohc_decomp_ctxt *const context,
                                              const struct rohc_rfc5225_decoded *const decoded,
                                              const size_t payload_len,
                                              bool *const do_change_mode)
	__attribute__((nonnull(1, 2, 4)));

static bool decomp_rfc5225_ip_udp_rtp_attempt_repair(const struct rohc_decomp *const decomp,
                                                 const struct rohc_decomp_ctxt *const context,
                                                 const struct rohc_ts pkt_arrival_time,
                                                 struct rohc_decomp_crc_corr_ctxt *const crc_corr,
                                                 void *const extr_bits)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 5)));

static uint32_t decomp_rfc5225_ip_udp_rtp_get_sn(const struct rohc_decomp_ctxt *const context)
	__attribute__((warn_unused_result, nonnull(1), pure));


/*
 * Definitions of private functions
 */

/**
 * @brief Create the ROHCv2 IP/UDP volatile and persistent parts of the context
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context            The decompression context
 * @param[out] persist_ctxt  The persistent part of the decompression context
 * @param[out] volat_ctxt    The volatile part of the decompression context
 * @return                   true if the ROHCv2 IP/UDP context was successfully
 *                           created, false if a problem occurred
 */
static bool decomp_rfc5225_ip_udp_rtp_new_context(const struct rohc_decomp_ctxt *const context,
                                              void **const persist_ctxt,
                                              struct rohc_decomp_volat_ctxt *const volat_ctxt)
{
	struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *rfc5225_ctxt;

	/* allocate memory for the context */
	*persist_ctxt = calloc(1, sizeof(struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt));
	if((*persist_ctxt) == NULL)
	{
		rohc_error(context->decompressor, ROHC_TRACE_DECOMP, context->profile->id,
		           "not enough memory for the ROHCv2 IP/UDP decompression context");
		goto error;
	}
	rfc5225_ctxt = *persist_ctxt;

	/* create the LSB decoding context for the MSN */
	rohc_lsb_init(&rfc5225_ctxt->msn_lsb_ctxt, 16);
	/* create the LSB decoding context for the innermost IP-ID */
	rohc_lsb_init(&rfc5225_ctxt->ip_id_offset_lsb_ctxt, 16);

	/* by default, no reordering accepted on the channel */
	rfc5225_ctxt->reorder_ratio = ROHC_REORDERING_NONE;

	/* volatile part */
	volat_ctxt->crc.type = ROHC_CRC_TYPE_NONE;
	volat_ctxt->crc.bits_nr = 0;
	volat_ctxt->extr_bits = malloc(sizeof(struct rohc_rfc5225_bits));
	if(volat_ctxt->extr_bits == NULL)
	{
		rohc_decomp_warn(context, "failed to allocate memory for the volatile part "
		                 "of one of the ROHCv2 IP/UDP decompression context");
		goto destroy_context;
	}
	volat_ctxt->decoded_values = malloc(sizeof(struct rohc_rfc5225_decoded));
	if(volat_ctxt->decoded_values == NULL)
	{
		rohc_decomp_warn(context, "failed to allocate memory for the volatile part "
		                 "of one of the ROHCv2 IP/UDP decompression context");
		goto free_extr_bits;
	}

	return true;

free_extr_bits:
	zfree(volat_ctxt->extr_bits);
destroy_context:
	zfree(rfc5225_ctxt);
error:
	return false;
}


/**
 * @brief Destroy profile-specific data, nothing to destroy for the
 *        ROHCv2 IP/UDP profile
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param rfc5225_ctxt  The persistent decompression context for the IP/UDP profile
 * @param volat_ctxt    The volatile part of the decompression context
 */
static void decomp_rfc5225_ip_udp_rtp_free_context(struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt,
                                               const struct rohc_decomp_volat_ctxt *const volat_ctxt)
{
	/* free the ROHCv2 IP/UDP decompression context itself */
	free(rfc5225_ctxt);

	/* free the volatile part of the decompression context */
	free(volat_ctxt->decoded_values);
	free(volat_ctxt->extr_bits);
}


/**
 * @brief Detect the type of ROHC packet for the ROHCv2 IP/UDP profile
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context        The decompression context
 * @param rohc_packet    The ROHC packet
 * @param rohc_length    The length of the ROHC packet
 * @param large_cid_len  The length of the optional large CID field
 * @return               The packet type
 */
static rohc_packet_t decomp_rfc5225_ip_udp_rtp_detect_pkt_type(const struct rohc_decomp_ctxt *const context,
                                                           const uint8_t *const rohc_packet,
                                                           const size_t rohc_length,
                                                           const size_t large_cid_len __attribute__((unused)))
{
	rohc_packet_t type;

	/* at least one byte required to check discriminator byte in packet
	 * (already checked by rohc_decomp_find_context) */
	assert(rohc_length >= 1);

	rohc_decomp_debug(context, "try to determine the header from first byte "
	                  "0x%02x", rohc_packet[0]);

	if(GET_BIT_7(rohc_packet) == 0) /* 1-bit discriminator '0' */
	{
		type = ROHC_PACKET_PT_0_CRC3;
	}
	else if(GET_BIT_5_7(rohc_packet) == 0x04) /* 3-bit discriminator '100' */
	{
		type = ROHC_PACKET_NORTP_PT_0_CRC7;
	}
	else if(GET_BIT_5_7(rohc_packet) == 0x05) /* 3-bit discriminator '101' */
	{
		type = ROHC_PACKET_NORTP_PT_1_SEQ_ID;
	}
	else if(GET_BIT_5_7(rohc_packet) == 0x06) /* 3-bit discriminator '110' */
	{
		type = ROHC_PACKET_NORTP_PT_2_SEQ_ID;
	}
	else if(GET_BIT_0_7(rohc_packet) == 0xfa) /* 8-bit discriminator '11111010' */
	{
		type = ROHC_PACKET_CO_COMMON;
	}
	else if(rohc_packet[0] == ROHC_PACKET_TYPE_CO_REPAIR) /* 8-bit '11111011' */
	{
		type = ROHC_PACKET_CO_REPAIR;
	}
	else if(rohc_packet[0] == ROHC_PACKET_TYPE_IR) /* 8-bit '11111101' */
	{
		type = ROHC_PACKET_IR;
	}
	else
	{
		type = ROHC_PACKET_UNKNOWN;
	}

	return type;
}


/**
 * @brief Parse one ROHC packet for the ROHCv2 IP/UDP profile
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context              The decompression context
 * @param rohc_packet          The ROHC packet to parse
 * @param large_cid_len        The length of the optional large CID field
 * @param[in,out] packet_type  IN:  The type of the ROHC packet to parse
 *                             OUT: The type of the parsed ROHC packet
 * @param[out] extr_crc        The CRC bits extracted from the ROHC packet
 * @param[out] bits            The bits extracted from the ROHC packet
 * @param[out] rohc_hdr_len    The length of the ROHC header (in bytes)
 * @return                     true if parsing was successful,
 *                             false if packet was malformed
 */
static bool decomp_rfc5225_ip_udp_rtp_parse_pkt(const struct rohc_decomp_ctxt *const context,
                                            const struct rohc_buf rohc_packet,
                                            const size_t large_cid_len,
                                            rohc_packet_t *const packet_type,
                                            struct rohc_decomp_crc *const extr_crc,
                                            struct rohc_rfc5225_bits *const bits,
                                            size_t *const rohc_hdr_len)
{
	bool status = false;

	/* reset all extracted bits */
	decomp_rfc5225_ip_udp_rtp_reset_extr_bits(context, bits);

	if((*packet_type) == ROHC_PACKET_IR)
	{
		status = decomp_rfc5225_ip_udp_rtp_parse_ir(context, rohc_packet, large_cid_len,
		                                        extr_crc, bits, rohc_hdr_len);
	}
	else if((*packet_type) == ROHC_PACKET_CO_REPAIR)
	{
		status = decomp_rfc5225_ip_udp_rtp_parse_co_repair(context, rohc_packet, large_cid_len,
		                                               extr_crc, bits, rohc_hdr_len);
	}
	if(status == false)
	{
		rohc_decomp_warn(context, "failed to parse %s packet",
		                 rohc_get_packet_descr(*packet_type));
		goto error;
	}

	return true;

error:
	return false;
}


/**
 * @brief Reset the extracted bits for next parsing
 *
 * @param ctxt       The decompression context
 * @param[out] bits  The extracted bits to reset
 */
static void decomp_rfc5225_ip_udp_rtp_reset_extr_bits(const struct rohc_decomp_ctxt *const ctxt,
                                                  struct rohc_rfc5225_bits *const bits)
{
	const struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt =
		ctxt->persist_ctxt;
	size_t i;

	/* set every bits and sizes to 0 */
	for(i = 0; i < ROHC_MAX_IP_HDRS; i++)
	{
		bits->ip[i].version = 0;
		bits->ip[i].tos_tc_bits_nr = 0;
		bits->ip[i].id_behavior_nr = 0;
		bits->ip[i].id.bits_nr = 0;
		bits->ip[i].df_nr = 0;
		bits->ip[i].ttl_hl_nr = 0;
		bits->ip[i].proto_nr = 0;
		bits->ip[i].flowid_nr = 0;
		bits->ip[i].saddr_nr = 0;
		bits->ip[i].daddr_nr = 0;
		/* TODO: handle IPv6 extension headers */
	}
	bits->ip_nr = 0;
	bits->msn.bits_nr = 0;
	bits->reorder_ratio_nr = 0;
	bits->outer_ip_flag_nr = 0;
	bits->ctrl_crc.type = ROHC_CRC_TYPE_NONE;
	bits->ctrl_crc.bits_nr = 0;

	/* if context handled at least one packet, init the list of IP headers */
	if(ctxt->num_recv_packets >= 1)
	{
		for(i = 0; i < rfc5225_ctxt->ip_contexts_nr; i++)
		{
			bits->ip[i].version = rfc5225_ctxt->ip_contexts[i].version;
			bits->ip[i].proto = rfc5225_ctxt->ip_contexts[i].ctxt.vx.next_header;
			bits->ip[i].proto_nr = 8;
			/* TODO: handle IPv6 extension headers */
		}
		bits->ip_nr = rfc5225_ctxt->ip_contexts_nr;
	}

	/* default constant LSB shift parameters */
	bits->msn.p = ROHC_LSB_SHIFT_VAR;
}


/**
 * @brief Parse one IR packet for the ROHCv2 IP/UDP profile
 *
 * @param ctxt               The decompression context
 * @param rohc_pkt           The ROHC packet to decode
 * @param large_cid_len      The length of the optional large CID field
 * @param[out] extr_crc      The CRC extracted from the ROHC packet
 * @param[out] bits          The bits extracted from the ROHC packet
 * @param[out] rohc_hdr_len  The length of the ROHC header (in bytes)
 * @return                   true if parsing was successful,
 *                           false if packet was malformed
 */
static bool decomp_rfc5225_ip_udp_rtp_parse_ir(const struct rohc_decomp_ctxt *const ctxt,
                                           const struct rohc_buf rohc_pkt,
                                           const size_t large_cid_len,
                                           struct rohc_decomp_crc *const extr_crc,
                                           struct rohc_rfc5225_bits *const bits,
                                           size_t *const rohc_hdr_len)
{
	const uint8_t *remain_data = rohc_buf_data(rohc_pkt);
	size_t remain_len = rohc_pkt.len;
	size_t static_chain_len;
	size_t dyn_chain_len;

	/* skip (length checked in rohc_decomp_find_context):
	 * - the first byte of the ROHC packet
	 * - the large CID if any
	 * - the Profile byte */
	assert(remain_len >= (1 + large_cid_len + 1));
	remain_data += 1 + large_cid_len + 1;
	remain_len -= 1 + large_cid_len + 1;

	/* parse CRC */
	if(remain_len < 1)
	{
		rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for the "
		                 "CRC byte");
		goto error;
	}
	extr_crc->type = ROHC_CRC_TYPE_NONE;
	extr_crc->bits = remain_data[0];
	extr_crc->bits_nr = 8;
	remain_data++;
	remain_len--;

	/* parse static chain */
	if(!decomp_rfc5225_ip_udp_rtp_parse_static_chain(ctxt, remain_data, remain_len,
	                                             bits, &static_chain_len))
	{
		rohc_decomp_warn(ctxt, "failed to parse the static chain");
		goto error;
	}
	remain_data += static_chain_len;
	remain_len -= static_chain_len;

	/* parse dynamic chain */
	if(!decomp_rfc5225_ip_udp_rtp_parse_dyn_chain(ctxt, remain_data, remain_len,
	                                          bits, &dyn_chain_len))
	{
		rohc_decomp_warn(ctxt, "failed to parse the dynamic chain");
		goto error;
	}
	remain_data += dyn_chain_len;
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_len -= dyn_chain_len;
#endif

	*rohc_hdr_len = remain_data - rohc_buf_data(rohc_pkt);
	return true;

error:
	return false;
}

/**
 * @brief Parse one co_repair packet for the ROHCv2 IP/UDP profile
 *
 * @param ctxt               The decompression context
 * @param rohc_pkt           The ROHC packet to decode
 * @param large_cid_len      The length of the optional large CID field
 * @param[out] hdr_crc       The CRC over uncomp headers extracted from ROHC packet
 * @param[out] bits          The bits extracted from the ROHC packet
 * @param[out] rohc_hdr_len  The length of the ROHC header (in bytes)
 * @return                   true if parsing was successful,
 *                           false if packet was malformed
 */
static bool decomp_rfc5225_ip_udp_rtp_parse_co_repair(const struct rohc_decomp_ctxt *const ctxt,
                                                  const struct rohc_buf rohc_pkt,
                                                  const size_t large_cid_len,
                                                  struct rohc_decomp_crc *const hdr_crc,
                                                  struct rohc_rfc5225_bits *const bits,
                                                  size_t *const rohc_hdr_len)
{
	const uint8_t *remain_data = rohc_buf_data(rohc_pkt);
	size_t remain_len = rohc_pkt.len;
	size_t dyn_chain_len;

	/* check packet usage */
	assert(ctxt->state == ROHC_DECOMP_STATE_SC ||
	       ctxt->state == ROHC_DECOMP_STATE_FC);

	/* reject too small co_repair packets, the following fields are mandatory:
	 *  - 1-byte packet discriminator
	 *  - 0/1/2-byte large CID
	 *  - 1-byte r1/CRC-7
	 *  - 1-byte r2/CRC-3
	 */
	if(remain_len < (1 + large_cid_len + 2))
	{
		rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for discriminator "
		                 "byte, large CID bytes, and CRC-7/CRC-3 bytes");
		goto error;
	}

	/* discriminator (already checked during packet detection */
	assert(remain_data[0] == ROHC_PACKET_TYPE_CO_REPAIR);
	remain_data++;
	remain_len--;

	/* skip any large CID bytes */
	remain_data += large_cid_len;
	remain_len -= large_cid_len;

	/* parse CRC-7 over uncompressed headers and CRC-3 over control fields */
	{
		const co_repair_crc_t *const co_repair_crc = (co_repair_crc_t *) remain_data;

		/* reserved field r1 shall be zero */
		if(co_repair_crc->r1 != 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field r1 is 0x%x "
			                 "instead of 0", co_repair_crc->r1);
			goto error;
		}
		/* CRC-7 over uncompressed headers */
		hdr_crc->type = ROHC_CRC_TYPE_7;
		hdr_crc->bits = co_repair_crc->header_crc;
		hdr_crc->bits_nr = 7;

		/* reserved field r2 shall be zero */
		if(co_repair_crc->r2 != 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field r2 is 0x%x "
			                 "instead of 0", co_repair_crc->r2);
			goto error;
		}
		/* CRC-3 over control fields */
		bits->ctrl_crc.type = ROHC_CRC_TYPE_3;
		bits->ctrl_crc.bits = co_repair_crc->ctrl_crc;
		bits->ctrl_crc.bits_nr = 3;

		/* skip CRCs */
		remain_data += sizeof(co_repair_crc_t);
		remain_len -= sizeof(co_repair_crc_t);
	}

	/* parse dynamic chain */
	if(!decomp_rfc5225_ip_udp_rtp_parse_dyn_chain(ctxt, remain_data, remain_len,
	                                          bits, &dyn_chain_len))
	{
		rohc_decomp_warn(ctxt, "failed to parse the dynamic chain");
		goto error;
	}
	remain_data += dyn_chain_len;
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_len -= dyn_chain_len;
#endif

	*rohc_hdr_len = remain_data - rohc_buf_data(rohc_pkt);
	return true;

error:
	return false;
}

/**
 * @brief Parse the static chain of the IR packet
 *
 * @param ctxt             The decompression context
 * @param rohc_pkt         The remaining part of the ROHC packet
 * @param rohc_len         The remaining length (in bytes) of the ROHC packet
 * @param[out] bits        The bits extracted from the static chain
 * @param[out] parsed_len  The length (in bytes) of static chain in case of success
 * @return                 true in the static chain was successfully parsed,
 *                         false if the ROHC packet was malformed
 */
static bool decomp_rfc5225_ip_udp_rtp_parse_static_chain(const struct rohc_decomp_ctxt *const ctxt,
                                                     const uint8_t *const rohc_pkt,
                                                     const size_t rohc_len,
                                                     struct rohc_rfc5225_bits *const bits,
                                                     size_t *const parsed_len)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	size_t ip_hdrs_nr;
	bool is_innermost = false;
	int ret;

	(*parsed_len) = 0;

	/* parse static IP part (IPv4/IPv6 headers and extension headers) */
	ip_hdrs_nr = 0;
	do
	{
		struct rohc_rfc5225_ip_bits *const ip_bits = &(bits->ip[ip_hdrs_nr]);

		ret = decomp_rfc5225_ip_udp_rtp_parse_static_ip(ctxt, remain_data, remain_len,
		                                            ip_bits, &is_innermost);
		if(ret < 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: malformed IP static part");
			goto error;
		}
		rohc_decomp_debug(ctxt, "IPv%u static part is %d-byte length",
		                  ip_bits->version, ret);
		assert(remain_len >= ((size_t) ret));
		remain_data += ret;
		remain_len -= ret;
		(*parsed_len) += ret;

		ip_hdrs_nr++;
	}
	while(!is_innermost && ip_hdrs_nr < ROHC_MAX_IP_HDRS);

	if(!is_innermost && ip_hdrs_nr >= ROHC_MAX_IP_HDRS)
	{
		rohc_decomp_warn(ctxt, "too many IP headers to decompress");
		goto error;
	}
	bits->ip_nr = ip_hdrs_nr;

	/* parse static UDP part */
	ret = decomp_rfc5225_ip_udp_rtp_parse_static_udp(ctxt, remain_data,remain_len,bits);
	if(ret < 0)
	{
		rohc_decomp_warn(ctxt, "malformed ROHC packet: malformed UDP static part");
		goto error;
	}
	rohc_decomp_debug(ctxt, "UDP static part is %d-byte length",ret);
	assert(remain_len >= ((size_t) ret));
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_data += ret;
	remain_len -= ret;
#endif
	(*parsed_len) += ret;

	return true;

error:
	return false;
}


/**
 * @brief Decode the static IP header of the ROHC packet
 *
 * @param ctxt               The decompression context
 * @param rohc_pkt           The remaining part of the ROHC packet
 * @param rohc_len           The remaining length (in bytes) of the ROHC packet
 * @param[out] ip_bits       The bits extracted from the IP part of the static chain
 * @param[out] is_innermost  Whether the IP header is the innermost IP header
 * @return                   The length of static IP header in case of success,
 *                           -1 if an error occurs
 */
static int decomp_rfc5225_ip_udp_rtp_parse_static_ip(const struct rohc_decomp_ctxt *const ctxt,
                                                 const uint8_t *const rohc_pkt,
                                                 const size_t rohc_len,
                                                 struct rohc_rfc5225_ip_bits *const ip_bits,
                                                 bool *const is_innermost)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	size_t read = 0;

	rohc_decomp_debug(ctxt, "parse IP static part");

	/* at least 1 byte required to read the version flag */
	if(remain_len < 1)
	{
		rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for the "
		                 "version flag of the IP static part");
		goto error;
	}

	/* parse IPv4 static part or IPv6 static part? */
	if(GET_BIT_7(remain_data) == 0)
	{
		const ipv4_static_t *const ipv4_static = (ipv4_static_t *) remain_data;

		rohc_decomp_debug(ctxt, "  IPv4 static part");
		ip_bits->version = IPV4;

		if(remain_len < sizeof(ipv4_static_t))
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for the "
			                 "IPv4 static part");
			goto error;
		}

		*is_innermost = !!(ipv4_static->innermost_ip == 1);
		if(ipv4_static->reserved != 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field is not "
			                 "zero, but 0x%x", ipv4_static->reserved);
			goto error;
		}
		ip_bits->proto = ipv4_static->protocol;
		ip_bits->proto_nr = 8;
		memcpy(ip_bits->saddr, &ipv4_static->src_addr, sizeof(uint32_t));
		ip_bits->saddr_nr = 32;
		memcpy(ip_bits->daddr, &ipv4_static->dst_addr, sizeof(uint32_t));
		ip_bits->daddr_nr = 32;

		/* IP extension headers not supported for IPv4 */
		/* TODO: handle IP extension headers */

		read += sizeof(ipv4_static_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
		remain_data += sizeof(ipv4_static_t);
		remain_len -= sizeof(ipv4_static_t);
#endif
	}
	else
	{
		rohc_decomp_debug(ctxt, "  IPv6 static part");
		ip_bits->version = IPV6;

		/* static with or without flow label? */
		if(GET_BIT_4(remain_data) == 0)
		{
			const ipv6_static_nofl_t *const ipv6_static =
				(ipv6_static_nofl_t *) remain_data;

			if(remain_len < sizeof(ipv6_static_nofl_t))
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for "
				                 "the IPv6 static part");
				goto error;
			}

			*is_innermost = !!(ipv6_static->innermost_ip == 1);
			if(ipv6_static->reserved1 != 0)
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field #1 is "
				                 "not zero, but 0x%x", ipv6_static->reserved1);
				goto error;
			}
			if(ipv6_static->reserved2 != 0)
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field #2 is "
				                 "not zero, but 0x%x", ipv6_static->reserved2);
				goto error;
			}
			ip_bits->flowid = 0;
			ip_bits->flowid_nr = 20;
			ip_bits->proto = ipv6_static->next_header;
			ip_bits->proto_nr = 8;
			memcpy(ip_bits->saddr, &ipv6_static->src_addr, sizeof(uint32_t) * 4);
			ip_bits->saddr_nr = 128;
			memcpy(ip_bits->daddr, &ipv6_static->dst_addr, sizeof(uint32_t) * 4);
			ip_bits->daddr_nr = 128;

			read += sizeof(ipv6_static_nofl_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
			remain_data += sizeof(ipv6_static_nofl_t);
			remain_len -= sizeof(ipv6_static_nofl_t);
#endif
		}
		else
		{
			const ipv6_static_fl_t *const ipv6_static =
				(ipv6_static_fl_t *) remain_data;

			if(remain_len < sizeof(ipv6_static_fl_t))
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for "
				                 "the IPv6 static part");
				goto error;
			}

			*is_innermost = !!(ipv6_static->innermost_ip == 1);
			if(ipv6_static->reserved != 0)
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field is "
				                 "not zero, but 0x%x", ipv6_static->reserved);
				goto error;
			}
			ip_bits->flowid = (ipv6_static->flow_label_msb << 16) |
			                  rohc_ntoh16(ipv6_static->flow_label_lsb);
			assert((ip_bits->flowid & 0xfffff) == ip_bits->flowid);
			rohc_decomp_debug(ctxt, "  IPv6 flow label = 0x%05x", ip_bits->flowid);
			ip_bits->flowid_nr = 20;
			ip_bits->proto = ipv6_static->next_header;
			ip_bits->proto_nr = 8;
			memcpy(ip_bits->saddr, &ipv6_static->src_addr, sizeof(uint32_t) * 4);
			ip_bits->saddr_nr = 128;
			memcpy(ip_bits->daddr, &ipv6_static->dst_addr, sizeof(uint32_t) * 4);
			ip_bits->daddr_nr = 128;

			read += sizeof(ipv6_static_fl_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
			remain_data += sizeof(ipv6_static_fl_t);
			remain_len -= sizeof(ipv6_static_fl_t);
#endif
		}

		/* TODO: handle IPv6 extension headers */
	}
	rohc_decomp_dump_buf(ctxt, "IP static part", rohc_pkt, read);

	return read;

error:
	return -1;
}


/**
 * @brief Parse the UDP static part of the ROHC packet
 *
 * @param ctxt      The decompression context
 * @param rohc_pkt  The ROHC packet to decode
 * @param rohc_len  The length of the ROHC packet
 * @param bits      OUT: The bits extracted from the ROHC header
 * @return          The number of bytes read in the ROHC packet,
 *                  -1 in case of failure
 */
static int decomp_rfc5225_ip_udp_rtp_parse_static_udp(const struct rohc_decomp_ctxt *const ctxt,
                                                  const uint8_t *rohc_pkt,
                                                  const size_t rohc_len,
                                                  struct rohc_rfc5225_bits *const bits)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	const udp_static_t *const udp_static = (udp_static_t *) remain_data;
	size_t size = 0;

	/* check the minimal length to parse the UDP static part */
	if(remain_len < sizeof(udp_static_t))
	{
		rohc_decomp_warn(ctxt, "ROHC packet too small (len = %zu)", remain_len);
		goto error;
	}

	bits->udp_sport = rohc_ntoh16(udp_static->src_port);
	bits->udp_sport_nr = 16;
	rohc_decomp_debug(ctxt, "UDP source port = %u", bits->udp_sport);

	bits->udp_dport = rohc_ntoh16(udp_static->dst_port);
	bits->udp_dport_nr = 16;
	rohc_decomp_debug(ctxt, "UDP destination port = %u", bits->udp_dport);

	size += sizeof(udp_static_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_data += sizeof(udp_static_t);
	remain_len -= sizeof(udp_static_t);
#endif

	rohc_decomp_dump_buf(ctxt, "UDP static part", rohc_pkt, size);
	return size;

error:
	return -1;
}


/**
 * @brief Parse the dynamic chain of the IR packet
 *
 * @param ctxt             The decompression context
 * @param rohc_pkt         The remaining part of the ROHC packet
 * @param rohc_len         The remaining length (in bytes) of the ROHC packet
 * @param[out] parsed_len  The length (in bytes) of static chain in case of success
 * @param[out] bits        The bits extracted from the dynamic chain
 * @return                 true in the dynamic chain was successfully parsed,
 *                         false if the ROHC packet was malformed
 */
static bool decomp_rfc5225_ip_udp_rtp_parse_dyn_chain(const struct rohc_decomp_ctxt *const ctxt,
                                                  const uint8_t *const rohc_pkt,
                                                  const size_t rohc_len,
                                                  struct rohc_rfc5225_bits *const bits,
                                                  size_t *const parsed_len)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	size_t ip_hdrs_nr;
	int ret;

	(*parsed_len) = 0;

	/* parse dynamic IP part (IPv4/IPv6 headers and extension headers) */
	assert(bits->ip_nr > 0);
	for(ip_hdrs_nr = 0; ip_hdrs_nr < bits->ip_nr; ip_hdrs_nr++)
	{
		struct rohc_rfc5225_ip_bits *const ip_bits = &(bits->ip[ip_hdrs_nr]);

		ret = decomp_rfc5225_ip_udp_rtp_parse_dyn_ip(ctxt, remain_data, remain_len,
		                                         ip_bits);
		if(ret < 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: malformed IP dynamic part");
			goto error;
		}
		rohc_decomp_debug(ctxt, "IPv%u dynamic part is %d-byte length",
		                  ip_bits->version, ret);
		assert(remain_len >= ((size_t) ret));
		remain_data += ret;
		remain_len -= ret;
		(*parsed_len) += ret;
	}

	/* parse dynamic UDP part */
	ret = decomp_rfc5225_ip_udp_rtp_parse_dyn_udp(ctxt, remain_data, remain_len, bits);
	if(ret < 0)
	{
		rohc_decomp_warn(ctxt, "malformed ROHC packet: malformed UDP dynamic part");
		goto error;
	}
	rohc_decomp_debug(ctxt, "UDP dynamic part is %d-byte length", ret);
	assert(remain_len >= ((size_t) ret));
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_data += ret;
	remain_len -= ret;
#endif
	(*parsed_len) += ret;

	return true;

error:
	return false;
}


/**
 * @brief Decode the dynamic IP header of the ROHC packet
 *
 * @param ctxt           The decompression context
 * @param rohc_pkt       The remaining part of the ROHC packet
 * @param rohc_len       The remaining length (in bytes) of the ROHC packet
 * @param[out] ip_bits   The bits extracted from the IP part of the dynamic chain
 * @return               The length of dynamic IP header in case of success,
 *                       -1 if an error occurs
 */
static int decomp_rfc5225_ip_udp_rtp_parse_dyn_ip(const struct rohc_decomp_ctxt *const ctxt,
                                              const uint8_t *const rohc_pkt,
                                              const size_t rohc_len,
                                              struct rohc_rfc5225_ip_bits *const ip_bits)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	size_t size = 0;

	rohc_decomp_debug(ctxt, "parse IP dynamic part");

	if(ip_bits->version == IPV4)
	{
		const ipv4_regular_dynamic_noipid_t *const ipv4_dynamic =
			(ipv4_regular_dynamic_noipid_t *) remain_data;

		if(remain_len < sizeof(ipv4_regular_dynamic_noipid_t))
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for "
			                 "IPv4 dynamic part");
			goto error;
		}

		if(ipv4_dynamic->reserved != 0)
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: reserved field is "
			                 "not zero, but 0x%x", ipv4_dynamic->reserved);
			goto error;
		}
		ip_bits->df = ipv4_dynamic->df;
		ip_bits->df_nr = 1;
		ip_bits->id_behavior = ipv4_dynamic->ip_id_behavior;
		ip_bits->id_behavior_nr = 2;
		rohc_decomp_debug(ctxt, "ip_id_behavior_innermost = %d",
		                  ip_bits->id_behavior);
		ip_bits->tos_tc_bits = ipv4_dynamic->tos_tc;
		ip_bits->tos_tc_bits_nr = 8;
		ip_bits->ttl_hl = ipv4_dynamic->ttl_hopl;
		ip_bits->ttl_hl_nr = 8;
		rohc_decomp_debug(ctxt, "TOS/TC = 0x%x, ttl_hopl = 0x%x",
		                  ip_bits->tos_tc_bits, ip_bits->ttl_hl);

		if(ipv4_dynamic->ip_id_behavior != ROHC_IP_ID_BEHAVIOR_ZERO)
		{
			const ipv4_regular_dynamic_ipid_t *const ipv4_dynamic_ipid =
					(ipv4_regular_dynamic_ipid_t *) remain_data;

			if(remain_len < sizeof(ipv4_regular_dynamic_ipid_t))
			{
				rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for "
				                 "IPv4 dynamic part");
				goto error;
			}

			ip_bits->id.bits = rohc_ntoh16(ipv4_dynamic_ipid->ip_id);
			ip_bits->id.bits_nr = 16;
			rohc_decomp_debug(ctxt, "IP-ID = 0x%04x", ip_bits->id.bits);

			size += sizeof(ipv4_regular_dynamic_ipid_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
			remain_data += sizeof(ipv4_regular_dynamic_ipid_t);
			remain_len -= sizeof(ipv4_regular_dynamic_ipid_t);
#endif
		}
		else
		{
			size += sizeof(ipv4_regular_dynamic_noipid_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
			remain_data += sizeof(ipv4_regular_dynamic_noipid_t);
			remain_len -= sizeof(ipv4_regular_dynamic_noipid_t);
#endif
		}
	}
	else /* IPv6 header */
	{
		const ipv6_regular_dynamic_t *const ipv6_dynamic =
			(ipv6_regular_dynamic_t *) remain_data;

		if(remain_len < sizeof(ipv6_regular_dynamic_t))
		{
			rohc_decomp_warn(ctxt, "malformed ROHC packet: too short for "
			                 "IPv6 dynamic part");
			goto error;
		}

		ip_bits->tos_tc_bits = ipv6_dynamic->tos_tc;
		ip_bits->tos_tc_bits_nr = 8;
		ip_bits->ttl_hl = ipv6_dynamic->ttl_hopl;
		ip_bits->ttl_hl_nr = 8;
		ip_bits->id_behavior = ROHC_IP_ID_BEHAVIOR_RAND;
		ip_bits->id_behavior_nr = 2;

		size += sizeof(ipv6_regular_dynamic_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
		remain_data += sizeof(ipv6_regular_dynamic_t);
		remain_len -= sizeof(ipv6_regular_dynamic_t);
#endif
	}

	rohc_decomp_dump_buf(ctxt, "IP dynamic part", rohc_pkt, size);

	return size;

error:
	return -1;
}


/**
 * @brief Parse the UDP dynamic part of the ROHC packet
 *
 * @param ctxt      The decompression context
 * @param rohc_pkt  The ROHC packet to decode
 * @param rohc_len  The length of the ROHC packet
 * @param bits      OUT: The bits extracted from the ROHC header
 * @return          The number of bytes read in the ROHC packet,
 *                  -1 in case of failure
 */
static int decomp_rfc5225_ip_udp_rtp_parse_dyn_udp(const struct rohc_decomp_ctxt *const ctxt,
                                               const uint8_t *rohc_pkt,
                                               const size_t rohc_len,
                                               struct rohc_rfc5225_bits *const bits)
{
	const uint8_t *remain_data = rohc_pkt;
	size_t remain_len = rohc_len;
	const udp_endpoint_dynamic_t *const udp_dynamic =
		(udp_endpoint_dynamic_t *) remain_data;
	size_t size = 0;

	/* check the minimal length to parse the UDP dynamic part */
	if(remain_len < sizeof(udp_endpoint_dynamic_t))
	{
		rohc_decomp_warn(ctxt, "ROHC packet too small (len = %zu)", remain_len);
		goto error;
	}

	bits->udp_checksum = rohc_ntoh16(udp_dynamic->checksum);
	bits->udp_checksum_nr = 16;
	rohc_decomp_debug(ctxt, "UDP checksum = 0x%04x", bits->udp_checksum);

	bits->msn.bits = rohc_ntoh16(udp_dynamic->msn);
	bits->msn.bits_nr = 16;
	rohc_decomp_debug(ctxt, "MSN = 0x%04x", bits->msn.bits);

	bits->reorder_ratio = udp_dynamic->reorder_ratio;
	bits->reorder_ratio_nr = 2;
	rohc_decomp_debug(ctxt, "reorder_ratio = %u", bits->reorder_ratio);

	size += sizeof(udp_endpoint_dynamic_t);
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	remain_data += sizeof(udp_endpoint_dynamic_t);
	remain_len -= sizeof(udp_endpoint_dynamic_t);
#endif

	rohc_decomp_dump_buf(ctxt, "UDP dynamic part", rohc_pkt, size);

	return size;

error:
	return -1;
}

/**
 * @brief Decode values from extracted bits for the ROHCv2 IP/UDP profile
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param ctxt          The decompression context
 * @param bits          The bits extracted from the ROHC packet
 * @param payload_len   The length of the packet payload (in bytes)
 * @param[out] decoded  The corresponding decoded values
 * @return              true if decoding is successful, false otherwise
 */
static bool decomp_rfc5225_ip_udp_rtp_decode_bits(const struct rohc_decomp_ctxt *const ctxt,
                                              const struct rohc_rfc5225_bits *const bits,
                                              const size_t payload_len __attribute__((unused)),
                                              struct rohc_rfc5225_decoded *const decoded)
{
	const struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt =
		ctxt->persist_ctxt;

	/* decode MSN */
	if(bits->msn.bits_nr == 16)
	{
		decoded->msn = bits->msn.bits;
		rohc_decomp_debug(ctxt, "decoded MSN = 0x%04x (%zu bits 0x%x)",
		                  decoded->msn, bits->msn.bits_nr, bits->msn.bits);
	}
	else
	{
		const rohc_lsb_shift_t p_computed =
			rohc_interval_get_rfc5225_msn_p(bits->msn.bits_nr, bits->reorder_ratio);
		uint32_t msn_decoded32;

		assert(bits->msn.bits_nr > 0); /* all packets contain some MSN bits */

		if(!rohc_lsb_decode(&rfc5225_ctxt->msn_lsb_ctxt, ROHC_LSB_REF_0, 0,
		                    bits->msn.bits, bits->msn.bits_nr, p_computed,
		                    &msn_decoded32))
		{
			rohc_decomp_warn(ctxt, "failed to decode %zu MSN bits 0x%x",
			                 bits->msn.bits_nr, bits->msn.bits);
			goto error;
		}
		decoded->msn = msn_decoded32 & 0xffff;
		rohc_decomp_debug(ctxt, "decoded MSN = 0x%04x (%zu bits 0x%x)",
		                  decoded->msn, bits->msn.bits_nr, bits->msn.bits);
	}

	/* decode UDP source port */
	if(bits->udp_sport_nr == 16)
	{
		decoded->udp_sport = bits->udp_sport;
		rohc_decomp_debug(ctxt, "decoded UDP source port = 0x%04x (%zu bits 0x%x)",
		                  decoded->udp_sport, bits->udp_sport_nr, bits->udp_sport);
	}
	else
	{
		decoded->udp_sport = rfc5225_ctxt->udp_sport;
		rohc_decomp_debug(ctxt, "UDP source port = 0x%04x taken from context",
		                  decoded->udp_sport);
	}

	/* decode UDP destination port */
	if(bits->udp_dport_nr == 16)
	{
		decoded->udp_dport = bits->udp_dport;
		rohc_decomp_debug(ctxt, "decoded UDP destination port = 0x%04x (%zu bits 0x%x)",
		                  decoded->udp_dport, bits->udp_dport_nr, bits->udp_dport);
	}
	else
	{
		decoded->udp_dport = rfc5225_ctxt->udp_dport;
		rohc_decomp_debug(ctxt, "UDP destination port = 0x%04x taken from context",
		                  decoded->udp_dport);
	}

	/* decode UDP checksum */
	if(bits->udp_checksum_nr == 16)
	{
		decoded->udp_checksum = bits->udp_checksum;
		rohc_decomp_debug(ctxt, "decoded UDP checksum = 0x%04x (%zu bits 0x%x)",
		                  decoded->udp_checksum, bits->udp_checksum_nr,
		                  bits->udp_checksum);

		/* is UDP checksum used? */
		decoded->udp_checksum_used = !!(decoded->udp_checksum != 0);
		rohc_decomp_debug(ctxt, "decoded UDP checksum used = %d",
		                  decoded->udp_checksum_used);
	}
	else
	{
		assert(bits->udp_checksum_nr == 0);
		assert(rfc5225_ctxt->udp_checksum_used == false);
		decoded->udp_checksum = 0;
		rohc_decomp_debug(ctxt, "decoded UDP checksum = 0 (not used)");
		decoded->udp_checksum_used = rfc5225_ctxt->udp_checksum_used;
		rohc_decomp_debug(ctxt, "decoded UDP checksum used = %d (taken from context)",
		                  decoded->udp_checksum_used);
	}

	/* decode reorder ratio */
	if(bits->reorder_ratio_nr > 0)
	{
		assert(bits->reorder_ratio_nr == 2);
		decoded->reorder_ratio = bits->reorder_ratio;
	}
	else
	{
		decoded->reorder_ratio = rfc5225_ctxt->reorder_ratio;
	}

	/* decode IP headers */
	if(!decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdrs(ctxt, bits, decoded))
	{
		rohc_decomp_warn(ctxt, "failed to decode bits extracted for IP headers");
		goto error;
	}

	/* all control fields were decoded, so let's check any CRC-3 computed over
	 * control fields */
	if(bits->ctrl_crc.type != ROHC_CRC_TYPE_NONE)
	{
		uint8_t ip_id_behaviors[ROHC_MAX_IP_HDRS];
		size_t ip_id_behaviors_nr;
		size_t ip_hdr_pos;
		uint8_t ctrl_crc_computed;

		assert(bits->ctrl_crc.type == ROHC_CRC_TYPE_3);
		assert(bits->ctrl_crc.bits_nr == 3);

		/* compute the CRC-3 over decoded control fields */
		assert(bits->ip_nr > 0);
		ip_id_behaviors_nr = 0;
		for(ip_hdr_pos = 0; ip_hdr_pos < bits->ip_nr; ip_hdr_pos++)
		{
			/* only IP-ID behavior of IPv4 headers are part of the CRC, see
			 * errata 2703 of RFC5225 for reasons to exclude IPv6 headers:
			 * https://www.rfc-editor.org/errata_search.php?rfc=5225&eid=2703 */
			if(rfc5225_ctxt->ip_contexts[ip_hdr_pos].ctxt.vx.version == IPV4)
			{
				ip_id_behaviors[ip_id_behaviors_nr] = bits->ip[ip_hdr_pos].id_behavior;
				rohc_decomp_debug(ctxt, "IP-ID behavior #%zu of IPv4 header #%zu "
				                  "= 0x%02x", ip_id_behaviors_nr + 1, ip_hdr_pos + 1,
				                  ip_id_behaviors[ip_id_behaviors_nr]);
				ip_id_behaviors_nr++;
			}
		}
		ctrl_crc_computed =
			compute_crc_ctrl_fields(ctxt->profile->id,
			                        ctxt->decompressor->crc_table_3,
			                        decoded->reorder_ratio, decoded->msn,
			                        ip_id_behaviors, ip_id_behaviors_nr);
		rohc_decomp_debug(ctxt, "CRC-3 on control fields = 0x%x (reorder_ratio = "
		                  "0x%02x, MSN = 0x%04x, %zu IP-ID behaviors)",
		                  ctrl_crc_computed, decoded->reorder_ratio, decoded->msn,
		                  ip_id_behaviors_nr);

		/* does the computed CRC match the one in packet? */
		if(ctrl_crc_computed != bits->ctrl_crc.bits)
		{
			rohc_decomp_warn(ctxt, "control CRC failure (computed = 0x%x, packet = "
			                 "0x%x)", ctrl_crc_computed, bits->ctrl_crc.bits);
			goto error;
		}
	}

	return true;

error:
	return false;

}


/**
 * @brief Decode values for all IP headers from extracted bits
 *
 * @param ctxt          The decompression context
 * @param bits          The bits extracted from the ROHC packet
 * @param[out] decoded  The corresponding decoded values
 * @return              true if decoding is successful, false otherwise
 */
static bool decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdrs(const struct rohc_decomp_ctxt *const ctxt,
                                                      const struct rohc_rfc5225_bits *const bits,
                                                      struct rohc_rfc5225_decoded *const decoded)
{
	const struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt =
		ctxt->persist_ctxt;
	size_t ip_hdr_nr;

	/* decode IP headers */
	assert(bits->ip_nr > 0);
	for(ip_hdr_nr = 0; ip_hdr_nr < bits->ip_nr; ip_hdr_nr++)
	{
		const struct rohc_rfc5225_ip_bits *const ip_bits = &(bits->ip[ip_hdr_nr]);
		const ip_context_t *const ip_ctxt = &(rfc5225_ctxt->ip_contexts[ip_hdr_nr]);
		struct rohc_rfc5225_decoded_ip *const ip_decoded = &(decoded->ip[ip_hdr_nr]);

		rohc_decomp_debug(ctxt, "decode fields of IP header #%zu", ip_hdr_nr + 1);

		if(!decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdr(ctxt, ip_bits, ip_ctxt,
		                                             decoded->msn, ip_decoded))
		{
			rohc_decomp_warn(ctxt, "failed to decode received bits for IP "
			                 "header #%zu", ip_hdr_nr + 1);
			goto error;
		}
	}
	decoded->ip_nr = bits->ip_nr;

	return true;

error:
	return false;
}


/**
 * @brief Decode values for one IP header from extracted bits
 *
 * @param ctxt             The decompression context
 * @param ip_bits          The IP bits extracted from the ROHC packet
 * @param ip_ctxt          The IP values recorded in context
 * @param decoded_msn      The decoded Master Sequence Number (MSN)
 * @param[out] ip_decoded  The corresponding decoded IP values
 * @return                 true if decoding is successful, false otherwise
 *
 * TODO: factorize with TCP profile
 */
static bool decomp_rfc5225_ip_udp_rtp_decode_bits_ip_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                     const struct rohc_rfc5225_ip_bits *const ip_bits,
                                                     const ip_context_t *const ip_ctxt,
                                                     const uint16_t decoded_msn,
                                                     struct rohc_rfc5225_decoded_ip *const ip_decoded)
{
	const struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt =
		ctxt->persist_ctxt;
	rohc_ip_id_behavior_t ip_id_behavior;

	/* version */
	ip_decoded->version = ip_bits->version;

	/* TOS/TC */
	if(ip_bits->tos_tc_bits_nr > 0)
	{
		assert(ip_bits->tos_tc_bits_nr == 8);
		ip_decoded->tos_tc = ip_bits->tos_tc_bits;
		rohc_decomp_debug(ctxt, "  decoded TOS/TC = 0x%02x (%zu bits 0x%x)",
		                  ip_decoded->tos_tc, ip_bits->tos_tc_bits_nr,
		                  ip_bits->tos_tc_bits);
	}
	else
	{
		ip_decoded->tos_tc = ip_ctxt->ctxt.vx.tos_tc;
		rohc_decomp_debug(ctxt, "  TOS/TC = 0x%02x taken from context",
		                  ip_decoded->tos_tc);
	}

	/* IP-ID behavior */
	if(ip_bits->id_behavior_nr > 0)
	{
		assert(ip_bits->id_behavior_nr == 2);
		ip_id_behavior = ip_bits->id_behavior;
		rohc_decomp_debug(ctxt, "  use behavior '%s' defined in current packet "
		                  "for IP-ID", rohc_ip_id_behavior_get_descr(ip_id_behavior));
	}
	else
	{
		ip_id_behavior = ip_ctxt->ctxt.vx.ip_id_behavior;
		rohc_decomp_debug(ctxt, "  use already-defined behavior '%s' for IP-ID",
		                  rohc_ip_id_behavior_get_descr(ip_id_behavior));
	}
	ip_decoded->id_behavior = ip_id_behavior;

	/* decode IP-ID according to its behavior */
	if(ip_bits->version == IPV4)
	{
		if(ip_bits->id.bits_nr == 16)
		{
			ip_decoded->id = ip_bits->id.bits;
			rohc_decomp_debug(ctxt, "  IP-ID = 0x%04x (%zu-bit 0x%x from packet)",
			                  ip_decoded->id, ip_bits->id.bits_nr, ip_bits->id.bits);
		}
		else if(ip_bits->id.bits_nr > 0)
		{
			/* ROHC packet cannot contain partial IP-ID if it is not sequential */
			if(ip_id_behavior > ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
			{
				rohc_decomp_warn(ctxt, "packet and context mismatch: received "
				                 "%zu bits of IP-ID in ROHC packet but IP-ID behavior "
				                 "is %s according to context", ip_bits->id.bits_nr,
				                 rohc_ip_id_behavior_get_descr(ip_id_behavior));
				goto error;
			}

			/* decode IP-ID from packet bits and context */
			if(!d_ip_id_lsb(ctxt, &rfc5225_ctxt->ip_id_offset_lsb_ctxt, decoded_msn,
			                ip_bits->id.bits, ip_bits->id.bits_nr, ip_bits->id.p,
			                &ip_decoded->id))
			{
				rohc_decomp_warn(ctxt, "failed to decode %zu IP-ID bits "
				                 "0x%x with p = %d", ip_bits->id.bits_nr,
				                 ip_bits->id.bits, ip_bits->id.p);
				goto error;
			}
			if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
			{
				ip_decoded->id = swab16(ip_decoded->id);
			}
			rohc_decomp_debug(ctxt, "  IP-ID = 0x%04x (decoded from "
			                  "%zu-bit 0x%x with p = %d)", ip_decoded->id,
			                  ip_bits->id.bits_nr, ip_bits->id.bits, ip_bits->id.p);
		}
		else /* inferred_sequential_ip_id */
		{
			if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_ZERO)
			{
				rohc_decomp_debug(ctxt, "  IP-ID follows a zero behavior");
				ip_decoded->id = 0;
			}
			else if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ ||
			        ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
			{
				const uint16_t last_msn =
					rohc_lsb_get_ref(&rfc5225_ctxt->msn_lsb_ctxt, ROHC_LSB_REF_0);
				const int16_t msn_delta = decoded_msn - last_msn;

				if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ)
				{
					ip_decoded->id = ip_ctxt->ctxt.v4.ip_id + msn_delta;
				}
				else /* ROHC_IP_ID_BEHAVIOR_SEQ_SWAP */
				{
					ip_decoded->id = swab16(swab16(ip_ctxt->ctxt.v4.ip_id) + msn_delta);
				}
				rohc_decomp_debug(ctxt, "  IP-ID = 0x%04x (inferred from context "
				                  "IP-ID 0x%x, and MSN %u -> %u)", ip_decoded->id,
				                  ip_ctxt->ctxt.v4.ip_id, last_msn, decoded_msn);
			}
			else
			{
				rohc_decomp_warn(ctxt, "packet and context mismatch: IP-ID is inferred "
				                 "but IP-ID behavior is %s according to context",
				                 rohc_ip_id_behavior_get_descr(ip_id_behavior));
				goto error;
			}
		}
	}
	else if(ip_bits->id.bits_nr > 0)
	{
		rohc_decomp_warn(ctxt, "packet and context mismatch: received %zu bits "
		                 "of IP-ID in ROHC packet but IP header is not IPv4 according "
		                 "to context", ip_bits->id.bits_nr);
		goto error;
	}

	/* decode TTL/HL */
	if(ip_bits->ttl_hl_nr == 8)
	{
		ip_decoded->ttl = ip_bits->ttl_hl;
		rohc_decomp_debug(ctxt, "  decoded TTL/HL = 0x%02x (%zu bits 0x%x)",
		                  ip_decoded->ttl, ip_bits->ttl_hl_nr, ip_bits->ttl_hl);
	}
	else
	{
		assert(ip_bits->ttl_hl_nr == 0);
		ip_decoded->ttl = ip_ctxt->ctxt.vx.ttl_hopl;
		rohc_decomp_debug(ctxt, "  TTL/HL = 0x%02x taken from context",
		                  ip_decoded->ttl);
	}

	/* change DF value if present in packet */
	if(ip_decoded->version == IPV4)
	{
		if(ip_bits->df_nr > 0)
		{
			assert(ip_bits->df_nr == 1);
			ip_decoded->df = ip_bits->df;
			rohc_decomp_debug(ctxt, "  decoded DF = %d", ip_decoded->df);
		}
		else
		{
			ip_decoded->df = ip_ctxt->ctxt.v4.df;
			rohc_decomp_debug(ctxt, "  DF = %d taken from context", ip_decoded->df);
		}
	}
	else if(ip_bits->df_nr > 0 && ip_bits->df != 0)
	{
		rohc_decomp_debug(ctxt, "malformed ROHC packet: DF shall be zero "
		                  "for innermost IPv6 header but it is %u", ip_bits->df);
#ifdef ROHC_RFC_STRICT_DECOMPRESSOR
		goto error;
#endif
	}

	/* protocol / next header */
	if(ip_bits->proto_nr > 0)
	{
		assert(ip_bits->proto_nr == 8);
		ip_decoded->proto = ip_bits->proto;
		rohc_decomp_debug(ctxt, "  decoded protocol/next header = 0x%02x (%d)",
		                  ip_decoded->proto, ip_decoded->proto);
	}
	else
	{
		ip_decoded->proto = ip_ctxt->ctxt.vx.next_header;
		rohc_decomp_debug(ctxt, "  protocol/next header = 0x%02x (%d) taken "
		                  "from context", ip_decoded->proto, ip_decoded->proto);
	}

	/* flow ID */
	if(ip_decoded->version == IPV6)
	{
		if(ip_bits->flowid_nr > 0)
		{
			assert(ip_bits->flowid_nr == 20);
			ip_decoded->flowid = ip_bits->flowid;
			rohc_decomp_debug(ctxt, "  decoded flow label = 0x%05x",
			                  ip_decoded->flowid);
		}
		else
		{
			ip_decoded->flowid = ip_ctxt->ctxt.v6.flow_label;
			rohc_decomp_debug(ctxt, "  flow label = 0x%05x taken from context",
			                  ip_decoded->flowid);
		}
	}
	else /* IPv4 */
	{
		assert(ip_bits->flowid_nr == 0);
	}

	/* source address */
	if(ip_bits->saddr_nr > 0)
	{
		memcpy(ip_decoded->saddr, ip_bits->saddr, ip_bits->saddr_nr / 8);
		rohc_decomp_debug(ctxt, "  %zu-byte source address (packet)",
		                  ip_bits->saddr_nr / 8);
	}
	else if(ip_decoded->version == IPV4)
	{
		memcpy(ip_decoded->saddr, &ip_ctxt->ctxt.v4.src_addr, 4);
		rohc_decomp_debug(ctxt, "  4-byte source address (context)");
	}
	else /* IPv6 */
	{
		memcpy(ip_decoded->saddr, ip_ctxt->ctxt.v6.src_addr, 16);
		rohc_decomp_debug(ctxt, "  16-byte source address (context)");
	}

	/* destination address */
	if(ip_bits->daddr_nr > 0)
	{
		memcpy(ip_decoded->daddr, ip_bits->daddr, ip_bits->daddr_nr / 8);
		rohc_decomp_debug(ctxt, "  %zu-byte destination address (packet)",
		                  ip_bits->daddr_nr / 8);
	}
	else if(ip_decoded->version == IPV4)
	{
		memcpy(ip_decoded->daddr, &ip_ctxt->ctxt.v4.dst_addr, 4);
		rohc_decomp_debug(ctxt, "  4-byte destination address (context)");
	}
	else /* IPv6 */
	{
		memcpy(ip_decoded->daddr, ip_ctxt->ctxt.v6.dest_addr, 16);
		rohc_decomp_debug(ctxt, "  16-byte destination address (context)");
	}

	/* TODO: handle IPv6 extension headers */

	return true;

error:
	return false;
}


/**
 * @brief Build the uncompressed headers for the ROHCv2 IP/UDP profile
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param decomp                The ROHC decompressor
 * @param context               The decompression context
 * @param packet_type           The type of ROHC packet
 * @param extr_crc              The CRC bits extracted from the ROHC header
 * @param decoded               The values decoded from ROHC header
 * @param payload_len           The length of the packet payload (in bytes)
 * @param[out] uncomp_hdrs      The uncompressed headers being built
 * @param[out] uncomp_hdrs_len  The length of the uncompressed headers written
 *                              into the buffer
 * @return                      Possible values:
 *                               \li ROHC_STATUS_OK if headers are built
 *                                   successfully,
 *                               \li ROHC_STATUS_OUTPUT_TOO_SMALL if
 *                                   \e uncomp_packet is too small
 */
static rohc_status_t decomp_rfc5225_ip_udp_rtp_build_hdrs(const struct rohc_decomp *const decomp,
                                                      const struct rohc_decomp_ctxt *const context,
                                                      const rohc_packet_t packet_type __attribute__((unused)),
                                                      const struct rohc_decomp_crc *const extr_crc,
                                                      const struct rohc_rfc5225_decoded *const decoded,
                                                      const size_t payload_len,
                                                      struct rohc_buf *const uncomp_hdrs,
                                                      size_t *const uncomp_hdrs_len)
{
	size_t ip_hdrs_len = 0;
	size_t ip_hdr_nr;
	size_t udp_hdr_len;

	rohc_decomp_debug(context, "build IP/UDP headers");

	*uncomp_hdrs_len = 0;

	/* build IP headers */
	if(!decomp_rfc5225_ip_udp_rtp_build_ip_hdrs(context, decoded, uncomp_hdrs,
	                                        &ip_hdrs_len))
	{
		rohc_decomp_warn(context, "failed to build uncompressed IP headers");
		goto error_output_too_small;
	}
	*uncomp_hdrs_len += ip_hdrs_len;

	/* build UDP header */
	if(!decomp_rfc5225_ip_udp_rtp_build_udp_hdr(context, decoded, payload_len,
	                                        uncomp_hdrs, &udp_hdr_len))
	{
		rohc_decomp_warn(context, "failed to build uncompressed UDP header");
		goto error_output_too_small;
	}
	*uncomp_hdrs_len += udp_hdr_len;

	/* unhide the IP headers */
	rohc_buf_push(uncomp_hdrs, *uncomp_hdrs_len);

	/* compute payload lengths and checksums for all IP headers */
	rohc_decomp_debug(context, "compute lengths and checksums for the %zu IP "
	                  "headers", decoded->ip_nr);
	assert(decoded->ip_nr > 0);
	for(ip_hdr_nr = 0; ip_hdr_nr < decoded->ip_nr; ip_hdr_nr++)
	{
		const struct rohc_rfc5225_decoded_ip *const ip_decoded =
			&(decoded->ip[ip_hdr_nr]);

		rohc_decomp_debug(context, "  IP header #%zu:", ip_hdr_nr + 1);
		if(ip_decoded->version == IPV4)
		{
			const uint16_t ipv4_tot_len = uncomp_hdrs->len + payload_len;
			struct ipv4_hdr *const ipv4 =
				(struct ipv4_hdr *) rohc_buf_data(*uncomp_hdrs);
			ipv4->tot_len = rohc_hton16(ipv4_tot_len);
			rohc_decomp_debug(context, "    IP total length = 0x%04x (%u)",
			                  ipv4_tot_len, ipv4_tot_len);
			ipv4->check = 0;
			ipv4->check =
				ip_fast_csum(rohc_buf_data(*uncomp_hdrs), ipv4->ihl);
			rohc_decomp_debug(context, "    IP checksum = 0x%04x on %zu bytes",
			                  rohc_ntoh16(ipv4->check), ipv4->ihl * sizeof(uint32_t));
			rohc_buf_pull(uncomp_hdrs, ipv4->ihl * sizeof(uint32_t));
		}
		else
		{
			struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) rohc_buf_data(*uncomp_hdrs);
			rohc_buf_pull(uncomp_hdrs, sizeof(struct ipv6_hdr));
			ipv6->plen = rohc_hton16(uncomp_hdrs->len + payload_len);
			rohc_decomp_debug(context, "    IPv6 payload length = %u",
			                  rohc_ntoh16(ipv6->plen));
			/* TODO: handle IPv6 extension headers */
		}
	}
	/* unhide the IP headers */
	rohc_buf_push(uncomp_hdrs, ip_hdrs_len);

	/* compute CRC on uncompressed headers if asked */
	if(extr_crc->type != ROHC_CRC_TYPE_NONE)
	{
		const bool crc_ok =
			rohc_decomp_check_uncomp_crc(decomp, context, uncomp_hdrs,
			                             extr_crc->type, extr_crc->bits);
		if(!crc_ok)
		{
			rohc_decomp_warn(context, "CRC detected a decompression failure for "
			                 "packet of type %s in state %s and mode %s",
			                 rohc_get_packet_descr(packet_type),
			                 rohc_decomp_get_state_descr(context->state),
			                 rohc_get_mode_descr(context->mode));
			if((decomp->features & ROHC_DECOMP_FEATURE_DUMP_PACKETS) != 0)
			{
				rohc_dump_packet(decomp->trace_callback, decomp->trace_callback_priv,
				                 ROHC_TRACE_DECOMP, ROHC_TRACE_WARNING,
				                 "uncompressed headers", *uncomp_hdrs);
			}
			goto error_crc;
		}
	}

	if((decomp->features & ROHC_DECOMP_FEATURE_DUMP_PACKETS) != 0)
	{
		rohc_dump_packet(decomp->trace_callback, decomp->trace_callback_priv,
		                 ROHC_TRACE_DECOMP, ROHC_TRACE_DEBUG,
		                 "IP headers", *uncomp_hdrs);
	}

	return ROHC_STATUS_OK;

error_crc:
	return ROHC_STATUS_BAD_CRC;
error_output_too_small:
	return ROHC_STATUS_OUTPUT_TOO_SMALL;
}


/**
 * @brief Build all of the uncompressed IP headers
 *
 * Build all of the uncompressed IP headers - IPv4 or IPv6 - from the context
 * and packet information.
 *
 * @param ctxt              The decompression context
 * @param decoded           The values decoded from the ROHC packet
 * @param[out] uncomp_pkt   The uncompressed packet being built
 * @param[out] ip_hdrs_len  The length of all the IP headers (in bytes)
 * @return                  true if IP headers were successfully built,
 *                          false if the output \e uncomp_packet was not
 *                          large enough
 */
static bool decomp_rfc5225_ip_udp_rtp_build_ip_hdrs(const struct rohc_decomp_ctxt *const ctxt,
                                                const struct rohc_rfc5225_decoded *const decoded,
                                                struct rohc_buf *const uncomp_pkt,
                                                size_t *const ip_hdrs_len)
{
	size_t ip_hdr_nr;

	assert(decoded->ip_nr > 0);

	rohc_decomp_debug(ctxt, "build the %zu IP headers", decoded->ip_nr);

	*ip_hdrs_len = 0;
	for(ip_hdr_nr = 0; ip_hdr_nr < decoded->ip_nr; ip_hdr_nr++)
	{
		const struct rohc_rfc5225_decoded_ip *const ip_decoded =
			&(decoded->ip[ip_hdr_nr]);
		size_t ip_hdr_len = 0;

		if(!decomp_rfc5225_ip_udp_rtp_build_ip_hdr(ctxt, ip_decoded, uncomp_pkt,
		                                       &ip_hdr_len))
		{
			rohc_decomp_warn(ctxt, "failed to build uncompressed IP header #%zu",
			                 ip_hdr_nr + 1);
			goto error;
		}
		*ip_hdrs_len += ip_hdr_len;
	}

	return true;

error:
	return false;
}


/**
 * @brief Build one single uncompressed IP header
 *
 * Build one single uncompressed IP header - IPv4 or IPv6 - from the context
 * and packet information.
 *
 * @param ctxt             The decompression context
 * @param decoded          The values decoded from the ROHC packet
 * @param[out] uncomp_pkt  The uncompressed packet being built
 * @param[out] ip_hdr_len  The length of the IP header (in bytes)
 * @return                 true if IP header was successfully built,
 *                         false if the output \e uncomp_packet was not
 *                         large enough
 */
static bool decomp_rfc5225_ip_udp_rtp_build_ip_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                               const struct rohc_rfc5225_decoded_ip *const decoded,
                                               struct rohc_buf *const uncomp_pkt,
                                               size_t *const ip_hdr_len)
{
	if(decoded->version == IPV4)
	{
		if(!decomp_rfc5225_ip_udp_rtp_build_ipv4_hdr(ctxt, decoded, uncomp_pkt,
		                                         ip_hdr_len))
		{
			rohc_decomp_warn(ctxt, "failed to build uncompressed IPv4 header");
			goto error;
		}
	}
	else
	{
		if(!decomp_rfc5225_ip_udp_rtp_build_ipv6_hdr(ctxt, decoded, uncomp_pkt, ip_hdr_len))
		{
			rohc_decomp_warn(ctxt, "failed to build uncompressed IPv6 header");
			goto error;
		}
	}

	return true;

error:
	return false;
}


/**
 * @brief Build one single uncompressed IPv4 header
 *
 * Build one single uncompressed IPv4 header from the context and packet
 * information.
 *
 * @param ctxt             The decompression context
 * @param decoded          The values decoded from the ROHC packet
 * @param[out] uncomp_pkt  The uncompressed packet being built
 * @param[out] ip_hdr_len  The length of the IPv4 header (in bytes)
 * @return                 true if IPv4 header was successfully built,
 *                         false if the output \e uncomp_packet was not
 *                         large enough
 */
static bool decomp_rfc5225_ip_udp_rtp_build_ipv4_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                 const struct rohc_rfc5225_decoded_ip *const decoded,
                                                 struct rohc_buf *const uncomp_pkt,
                                                 size_t *const ip_hdr_len)
{
	struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) rohc_buf_data(*uncomp_pkt);
	const size_t hdr_len = sizeof(struct ipv4_hdr);

	rohc_decomp_debug(ctxt, "  build %zu-byte IPv4 header", hdr_len);

	if(rohc_buf_avail_len(*uncomp_pkt) < hdr_len)
	{
		rohc_decomp_warn(ctxt, "output buffer too small for the %zu-byte IPv4 "
		                 "header", hdr_len);
		goto error;
	}

	/* static part */
	ipv4->version = decoded->version;
	rohc_decomp_debug(ctxt, "    version = %u", ipv4->version);
	ipv4->ihl = hdr_len / sizeof(uint32_t);
	rohc_decomp_debug(ctxt, "    ihl = %u", ipv4->ihl);
	ipv4->protocol = decoded->proto;
	memcpy(&ipv4->saddr, decoded->saddr, 4);
	rohc_decomp_debug(ctxt, "    src addr = 0x%08x", rohc_hton32(ipv4->saddr));
	memcpy(&ipv4->daddr, decoded->daddr, 4);
	rohc_decomp_debug(ctxt, "    dst addr = 0x%08x", rohc_hton32(ipv4->daddr));

	/* dynamic part */
	ipv4->frag_off = 0;
	ipv4->df = decoded->df;
	ipv4->tos = decoded->tos_tc;
	ipv4->ttl = decoded->ttl;
	rohc_decomp_debug(ctxt, "    TOS = 0x%02x, TTL = %u", ipv4->tos, ipv4->ttl);
	/* IP-ID */
	ipv4->id = rohc_hton16(decoded->id);
	rohc_decomp_debug(ctxt, "    %s IP-ID = 0x%04x",
	                  rohc_ip_id_behavior_get_descr(decoded->id_behavior),
	                  rohc_ntoh16(ipv4->id));

	/* length and checksums will be computed once all headers are built */

	/* skip IPv4 header */
	uncomp_pkt->len += hdr_len;
	rohc_buf_pull(uncomp_pkt, hdr_len);
	*ip_hdr_len += hdr_len;

	return true;

error:
	return false;
}


/**
 * @brief Build one single uncompressed IPv6 header
 *
 * Build one single uncompressed IPv6 header - including IPv6 extension
 * headers - from the context and packet information.
 *
 * @param ctxt             The decompression context
 * @param decoded          The values decoded from the ROHC packet
 * @param[out] uncomp_pkt  The uncompressed packet being built
 * @param[out] ip_hdr_len  The length of the IPv6 header (in bytes)
 * @return                 true if IPv6 header was successfully built,
 *                         false if the output \e uncomp_packet was not
 *                         large enough
 */
static bool decomp_rfc5225_ip_udp_rtp_build_ipv6_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                 const struct rohc_rfc5225_decoded_ip *const decoded,
                                                 struct rohc_buf *const uncomp_pkt,
                                                 size_t *const ip_hdr_len)
{
	struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) rohc_buf_data(*uncomp_pkt);
	const size_t hdr_len = sizeof(struct ipv6_hdr);
	const size_t ipv6_exts_len = 0; /* TODO: handle IP extension headers */
	const size_t full_ipv6_len = hdr_len + ipv6_exts_len;

	rohc_decomp_debug(ctxt, "  build %zu-byte IPv6 header (with %zu bytes of "
	                  "extension headers)", full_ipv6_len, ipv6_exts_len);

	if(rohc_buf_avail_len(*uncomp_pkt) < full_ipv6_len)
	{
		rohc_decomp_warn(ctxt, "output buffer too small for the %zu-byte IPv6 "
		                 "header (with %zu bytes of extension headers)",
		                 full_ipv6_len, ipv6_exts_len);
		goto error;
	}

	/* static part */
	ipv6->version = decoded->version;
	rohc_decomp_debug(ctxt, "    version = %u", ipv6->version);
	ipv6_set_flow_label(ipv6, decoded->flowid);
	rohc_decomp_debug(ctxt, "    flow label = 0x%01x%04x",
	                  ipv6->flow1, rohc_ntoh16(ipv6->flow2));
	ipv6->nh = decoded->proto;
	memcpy(&ipv6->saddr, decoded->saddr, sizeof(struct ipv6_addr));
	memcpy(&ipv6->daddr, decoded->daddr, sizeof(struct ipv6_addr));

	/* dynamic part */
	ipv6_set_tc(ipv6, decoded->tos_tc);
	ipv6->hl = decoded->ttl;
	rohc_decomp_debug(ctxt, "    TC = 0x%02x, HL = %u", decoded->tos_tc, ipv6->hl);

	/* total length will be computed once all headers are built */

	/* skip IPv6 header */
	uncomp_pkt->len += hdr_len;
	rohc_buf_pull(uncomp_pkt, hdr_len);
	*ip_hdr_len += hdr_len;

	/* TODO: handle IP extension headers */

	return true;

error:
	return false;
}


/**
 * @brief Build the uncompressed UDP header
 *
 * Build the uncompressed UDP header from the context and packet information.
 *
 * @param ctxt              The decompression context
 * @param decoded           The values decoded from the ROHC packet
 * @param payload_len       The length (in bytes) of the payload
 * @param[out] uncomp_pkt   The uncompressed packet being built
 * @param[out] udp_hdr_len  The length of the UDP header (in bytes)
 * @return                  true if UDP header was successfully built,
 *                          false if the output \e uncomp_packet was not
 *                          large enough
 */
static bool decomp_rfc5225_ip_udp_rtp_build_udp_hdr(const struct rohc_decomp_ctxt *const ctxt,
                                                const struct rohc_rfc5225_decoded *const decoded,
                                                const size_t payload_len,
                                                struct rohc_buf *const uncomp_pkt,
                                                size_t *const udp_hdr_len)
{
	struct udphdr *const udp = (struct udphdr *) rohc_buf_data(*uncomp_pkt);
	const size_t hdr_len = sizeof(struct udphdr);

	rohc_decomp_debug(ctxt, "  build %zu-byte UDP header", hdr_len);

	if(rohc_buf_avail_len(*uncomp_pkt) < hdr_len)
	{
		rohc_decomp_warn(ctxt, "output buffer too small for the %zu-byte UDP header",
		                 hdr_len);
		goto error;
	}

	/* static part */
	udp->source = rohc_hton16(decoded->udp_sport);
	rohc_decomp_debug(ctxt, "    source port = 0x%04x", rohc_ntoh16(udp->source));
	udp->dest = rohc_hton16(decoded->udp_dport);
	rohc_decomp_debug(ctxt, "    destination port = 0x%04x", rohc_ntoh16(udp->dest));

	/* dynamic part */
	udp->check = rohc_hton16(decoded->udp_checksum);
	rohc_decomp_debug(ctxt, "    checksum = 0x%04x", rohc_ntoh16(udp->check));

	/* inferred fields */
	udp->len = rohc_hton16(hdr_len + payload_len);
	rohc_decomp_debug(ctxt, "    length = 0x%04x", rohc_ntoh16(udp->len));

	/* skip UDP header */
	uncomp_pkt->len += hdr_len;
	rohc_buf_pull(uncomp_pkt, hdr_len);
	*udp_hdr_len = hdr_len;

	return true;

error:
	return false;
}


/**
 * @brief Update the decompression context with the infos of current packet
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context              The decompression context
 * @param decoded              The decoded values to update in the context
 * @param payload_len          The length of the packet payload (in bytes)
 * @param[out] do_change_mode  Whether the profile context wants to change
 *                             its operational mode or not
 */
static void decomp_rfc5225_ip_udp_rtp_update_ctxt(struct rohc_decomp_ctxt *const context,
                                              const struct rohc_rfc5225_decoded *const decoded,
                                              const size_t payload_len __attribute__((unused)),
                                              bool *const do_change_mode __attribute__((unused)))
{
	struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt = context->persist_ctxt;
	const uint16_t msn = decoded->msn;
	size_t ip_hdr_nr;

	/* MSN */
	rohc_lsb_set_ref(&rfc5225_ctxt->msn_lsb_ctxt, msn, false);
	rohc_decomp_debug(context, "MSN 0x%04x / %u is the new reference", msn, msn);

	/* update context for IP headers */
	assert(decoded->ip_nr > 0);
	for(ip_hdr_nr = 0; ip_hdr_nr < decoded->ip_nr; ip_hdr_nr++)
	{
		const struct rohc_rfc5225_decoded_ip *const ip_decoded =
			&(decoded->ip[ip_hdr_nr]);
		ip_context_t *const ip_context = &(rfc5225_ctxt->ip_contexts[ip_hdr_nr]);
		const bool is_inner = !!(ip_hdr_nr == (decoded->ip_nr - 1));

		rohc_decomp_debug(context, "update context for IPv%u header #%zu",
		                  ip_decoded->version, ip_hdr_nr + 1);

		ip_context->version = ip_decoded->version;
		ip_context->ctxt.vx.version = ip_decoded->version;
		ip_context->ctxt.vx.tos_tc = ip_decoded->tos_tc;
		ip_context->ctxt.vx.ttl_hopl = ip_decoded->ttl;
		ip_context->ctxt.vx.next_header = ip_decoded->proto;
		ip_context->ctxt.vx.ip_id_behavior = ip_decoded->id_behavior;

		if(ip_context->version == IPV4)
		{
			ip_context->ctxt.v4.df = ip_decoded->df;
			ip_context->ctxt.v4.ip_id = ip_decoded->id;
			memcpy(&ip_context->ctxt.v4.src_addr, ip_decoded->saddr, 4);
			memcpy(&ip_context->ctxt.v4.dst_addr, ip_decoded->daddr, 4);

			if(is_inner)
			{
				uint16_t ip_id_offset;
				if(ip_decoded->id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
				{
					ip_id_offset = swab16(ip_context->ctxt.v4.ip_id) - msn;
				}
				else
				{
					ip_id_offset = ip_context->ctxt.v4.ip_id - msn;
				}
				rohc_lsb_set_ref(&rfc5225_ctxt->ip_id_offset_lsb_ctxt,
				                 ip_id_offset, false);
				rohc_decomp_debug(context, "innermost IP-ID offset 0x%04x is the new "
				                  "reference", ip_id_offset);
			}

			/* TODO: extension headers */
		}
		else /* IPv6 */
		{
			assert((ip_decoded->flowid & 0xfffff) == ip_decoded->flowid);
			ip_context->ctxt.v6.flow_label = ip_decoded->flowid;
			memcpy(&ip_context->ctxt.v6.src_addr, ip_decoded->saddr, 16);
			memcpy(&ip_context->ctxt.v6.dest_addr, ip_decoded->daddr, 16);

			/* TODO: extension headers */
		}
	}
	rfc5225_ctxt->ip_contexts_nr = decoded->ip_nr;

	/* update context for the UDP header */
	rfc5225_ctxt->udp_checksum_used = decoded->udp_checksum_used;
}


/**
 * @brief Attempt a packet/context repair upon CRC failure
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param decomp             The ROHC decompressor
 * @param context            The decompression context
 * @param pkt_arrival_time   The arrival time of the ROHC packet that caused
 *                           the CRC failure
 * @param[in,out] crc_corr   The context for corrections upon CRC failures
 * @param[in,out] extr_bits  The bits extracted from the ROHC header
 * @return                   true if repair is possible, false if not
 */
static bool decomp_rfc5225_ip_udp_rtp_attempt_repair(const struct rohc_decomp *const decomp __attribute__((unused)),
                                                 const struct rohc_decomp_ctxt *const context __attribute__((unused)),
                                                 const struct rohc_ts pkt_arrival_time __attribute__((unused)),
                                                 struct rohc_decomp_crc_corr_ctxt *const crc_corr __attribute__((unused)),
                                                 void *const extr_bits __attribute__((unused)))
{
	/* there is no packet/context repair for ROHCv2 profiles */
	rohc_decomp_debug(context, "there is no packet/context repair for ROHCv2");
	return false;
}


/**
 * @brief Get the reference SN value of the context
 *
 * Always return 0 for the ROHCv2 IP/UDP profile.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The decompression context
 * @return        The reference SN value
 */
static uint32_t decomp_rfc5225_ip_udp_rtp_get_sn(const struct rohc_decomp_ctxt *const context)
{
	struct rohc_decomp_rfc5225_ip_udp_rtp_ctxt *const rfc5225_ctxt =
		context->persist_ctxt;
	const uint16_t msn =
		rohc_lsb_get_ref(&rfc5225_ctxt->msn_lsb_ctxt, ROHC_LSB_REF_0);
	rohc_decomp_debug(context, "MSN = %u (0x%x)", msn, msn);
	return msn;
}


/**
 * @brief Define the decompression part of the ROHCv2 IP/UDP profile as
 *        described in the RFC 5225
 */
const struct rohc_decomp_profile rohc_decomp_rfc5225_ip_udp_rtp_profile =
{
	.id              = ROHCv2_PROFILE_IP_UDP, /* profile ID (RFC5225, ROHCv2 IP/UDP) */
	.msn_max_bits    = 16,
	.new_context     = decomp_rfc5225_ip_udp_rtp_new_context,
	.free_context    = (rohc_decomp_free_context_t) decomp_rfc5225_ip_udp_rtp_free_context,
	.detect_pkt_type = decomp_rfc5225_ip_udp_rtp_detect_pkt_type,
	.parse_pkt       = (rohc_decomp_parse_pkt_t) decomp_rfc5225_ip_udp_rtp_parse_pkt,
	.decode_bits     = (rohc_decomp_decode_bits_t) decomp_rfc5225_ip_udp_rtp_decode_bits,
	.build_hdrs      = (rohc_decomp_build_hdrs_t) decomp_rfc5225_ip_udp_rtp_build_hdrs,
	.update_ctxt     = (rohc_decomp_update_ctxt_t) decomp_rfc5225_ip_udp_rtp_update_ctxt,
	.attempt_repair  = (rohc_decomp_attempt_repair_t) decomp_rfc5225_ip_udp_rtp_attempt_repair,
	.get_sn          = decomp_rfc5225_ip_udp_rtp_get_sn,
};

