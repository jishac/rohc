/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file c_rtp.c
 * @brief ROHC compression context for the RTP profile.
 * @author David Moreau from TAS
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 */

#include "c_rtp.h"
#include "c_udp.h"
#include "rohc_traces.h"
#include "rohc_packets.h"
#include "rohc_utils.h"
#include "sdvl.h"
#include "crc.h"

#include <stdlib.h>
#include <assert.h>


/*
 * Constants and macros
 */

/**
 * @brief The list of UDP ports associated with RTP streams
 *
 * The port numbers must be separated by a comma
 */
#define RTP_PORTS  1234, 36780, 33238, 5020, 5002


/*
 * Private function prototypes.
 */

static rohc_packet_t c_rtp_decide_FO_packet(const struct c_context *context);
static rohc_packet_t c_rtp_decide_SO_packet(const struct c_context *context);
static rohc_ext_t c_rtp_decide_extension(const struct c_context *context);

static uint16_t c_rtp_get_next_sn(const struct c_context *context,
                                  const struct ip_packet *outer_ip,
                                  const struct ip_packet *inner_ip);

int rtp_code_static_rtp_part(const struct c_context *context,
                             const unsigned char *next_header,
                             unsigned char *const dest,
                             int counter);

int rtp_code_dynamic_rtp_part(const struct c_context *context,
                              const unsigned char *next_header,
                              unsigned char *const dest,
                              int counter);

int rtp_changed_rtp_dynamic(const struct c_context *context,
                            const struct udphdr *udp);


/**
 * @brief Create a new RTP context and initialize it thanks to the given
 *        IP/UDP/RTP packet.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The compression context
 * @param ip      The IP/UDP/RTP packet given to initialize the new context
 * @return        1 if successful, 0 otherwise
 */
int c_rtp_create(struct c_context *const context, const struct ip_packet *ip)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	struct ip_packet ip2;
	const struct ip_packet *last_ip_header;
	const struct udphdr *udp;
	const struct rtphdr *rtp;
	unsigned int ip_proto;

	/* create and initialize the generic part of the profile context */
	if(!c_generic_create(context, ip))
	{
		rohc_debugf(0, "generic context creation failed\n");
		goto quit;
	}
	g_context = (struct c_generic_context *) context->specific;

	/* check if packet is IP/UDP/RTP or IP/IP/UDP/RTP */
	ip_proto = ip_get_protocol(ip);
	if(ip_proto == IPPROTO_IPIP || ip_proto == IPPROTO_IPV6)
	{
		/* get the last IP header */
		if(!ip_get_inner_packet(ip, &ip2))
		{
			rohc_debugf(0, "cannot create the inner IP header\n");
			goto clean;
		}
		last_ip_header = &ip2;

		/* get the transport protocol */
		ip_proto = ip_get_protocol(last_ip_header);
	}
	else
	{
		/* only one single IP header, the last IP header is the first one */
		last_ip_header = ip;
	}

	if(ip_proto != IPPROTO_UDP)
	{
		rohc_debugf(0, "next header is not UDP (%d), cannot use this profile\n",
		            ip_proto);
		goto clean;
	}

	udp = (struct udphdr *) ip_get_next_layer(last_ip_header);
	rtp = (struct rtphdr *) (udp + 1);

	/* initialize SN with the SN found in the RTP header */
	g_context->sn = ntohs(rtp->sn);
	rohc_debugf(1, "initialize context(SN) = hdr(SN) of first packet = %u\n",
	            g_context->sn);

	/* create the RTP part of the profile context */
	rtp_context = malloc(sizeof(struct sc_rtp_context));
	if(rtp_context == NULL)
	{
		rohc_debugf(0, "no memory for the RTP part of the profile context\n");
		goto clean;
	}
	g_context->specific = rtp_context;

	/* initialize the RTP part of the profile context */
	rtp_context->udp_checksum_change_count = 0;
	rtp_context->old_udp = *udp;
	rtp_context->rtp_pt_change_count = 0;
	rtp_context->old_rtp = *rtp;
	if(!c_create_sc(&rtp_context->ts_sc,
	                context->compressor->wlsb_window_width))
	{
		rohc_debugf(0, "cannot create scaled RTP Timestamp encoding\n");
		goto clean;
	}

	/* init the RTP-specific temporary variables */
	rtp_context->tmp.send_rtp_dynamic = -1;
	rtp_context->tmp.timestamp = 0;
	rtp_context->tmp.ts_send = 0;
	/* do not transmit any RTP TimeStamp (TS) bit by default */
	rtp_context->tmp.nr_ts_bits = 0;
	/* RTP Marker (M) bit is not set by default */
	rtp_context->tmp.m_set = 0;
	rtp_context->tmp.rtp_pt_changed = 0;

	/* init the RTP-specific variables and functions */
	g_context->next_header_proto = IPPROTO_UDP;
	g_context->next_header_len = sizeof(struct udphdr) + sizeof(struct rtphdr);
	g_context->decide_state = rtp_decide_state;
	g_context->decide_FO_packet = c_rtp_decide_FO_packet;
	g_context->decide_SO_packet = c_rtp_decide_SO_packet;
	g_context->decide_extension = c_rtp_decide_extension;
	g_context->init_at_IR = NULL;
	g_context->get_next_sn = c_rtp_get_next_sn;
	g_context->code_static_part = rtp_code_static_rtp_part;
	g_context->code_dynamic_part = rtp_code_dynamic_rtp_part;
	g_context->code_UO_packet_head = NULL;
	g_context->code_UO_packet_tail = udp_code_UO_packet_tail;
	g_context->compute_crc_static = rtp_compute_crc_static;
	g_context->compute_crc_dynamic = rtp_compute_crc_dynamic;

	return 1;

clean:
	c_generic_destroy(context);
quit:
	return 0;
}


/**
 * @brief Destroy the RTP context.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The RTP compression context to destroy
 */
void c_rtp_destroy(struct c_context *const context)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;

	assert(context != NULL);
	assert(context->specific != NULL);
	g_context = (struct c_generic_context *) context->specific;
	assert(g_context->specific != NULL);
	rtp_context = (struct sc_rtp_context *) g_context->specific;

	c_destroy_sc(&rtp_context->ts_sc);
	c_generic_destroy(context);
}


/**
 * @brief Check if the IP/UDP/RTP packet belongs to the context
 *
 * Conditions are:
 *  - the number of IP headers must be the same as in context
 *  - IP version of the two IP headers must be the same as in context
 *  - IP packets must not be fragmented
 *  - the source and destination addresses of the two IP headers must match the
 *    ones in the context
 *  - the transport protocol must be UDP
 *  - the source and destination ports of the UDP header must match the ones in
 *    the context
 *  - IPv6 only: the Flow Label of the two IP headers must match the ones the
 *    context
 *  - the SSRC field of the RTP header must match the one in the context
 *
 * All the context but the last one are done by the c_udp_check_context()
 * function.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The compression context
 * @param ip      The IP/UDP/RTP packet to check
 * @return        1 if the IP/UDP/RTP packet belongs to the context,
 *                0 if it does not belong to the context and
 *                -1 if an error occurs
 *
 * @see c_udp_check_context
 */
int c_rtp_check_context(const struct c_context *context,
                        const struct ip_packet *ip)
{
	const struct c_generic_context *g_context;
	const struct sc_rtp_context *rtp_context;
	struct ip_packet ip2;
	const struct ip_packet *last_ip_header;
	const struct udphdr *udp;
	const struct rtphdr *rtp;
	unsigned int ip_proto;
	int udp_check;
	int is_rtp_same;

	/* check IP and UDP headers */
	udp_check = c_udp_check_context(context, ip);
	if(udp_check != 1)
	{
		goto quit;
	}

	/* get the last IP header */
	ip_proto = ip_get_protocol(ip);
	if(ip_proto == IPPROTO_IPIP || ip_proto == IPPROTO_IPV6)
	{
		/* second IP header is last IP header */
		if(!ip_get_inner_packet(ip, &ip2))
		{
			rohc_debugf(0, "cannot create the inner IP header\n");
			goto error;
		}
		last_ip_header = &ip2;
	}
	else
	{
		/* first IP header is last IP header */
		last_ip_header = ip;
	}

	/* get UDP and RTP headers */
	udp = (struct udphdr *) ip_get_next_layer(last_ip_header);
	rtp = (struct rtphdr *) (udp + 1);

	/* check the RTP SSRC field */
	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;
	is_rtp_same = (rtp_context->old_rtp.ssrc == rtp->ssrc);

	return is_rtp_same;

quit:
	return udp_check;
error:
	return -1;
}


/**
 * @brief Decide which packet to send when in First Order (FO) state.
 *
 * Packets that can be used are the IR-DYN and UO-2 packets.
 *
 * @see decide_packet
 *
 * @param context The compression context
 * @return        The packet type among PACKET_IR_DYN and PACKET_UOR_2
 */
static rohc_packet_t c_rtp_decide_FO_packet(const struct c_context *context)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	rohc_packet_t packet;
	int nr_of_ip_hdr;
	size_t nr_sn_bits;
	size_t nr_ts_bits;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;
	nr_of_ip_hdr = g_context->tmp.nr_of_ip_hdr;
	nr_sn_bits = g_context->tmp.nr_sn_bits;
	nr_ts_bits = rtp_context->tmp.nr_ts_bits;

	if(g_context->tmp.send_static)
	{
		g_context->ir_dyn_count = 0;
		packet = PACKET_UOR_2_RTP;
		rohc_debugf(3, "choose packet UOR-2-RTP because at least one static "
		            "field changed\n");
	}
	else if(g_context->ir_dyn_count < MAX_FO_COUNT)
	{
		g_context->ir_dyn_count++;
		packet = PACKET_IR_DYN;
		rohc_debugf(3, "choose packet IR-DYN because not enough IR-DYN "
		            "packets were transmitted yet (%d / %d)\n",
		            g_context->ir_dyn_count, MAX_FO_COUNT);
	}
	else if(nr_of_ip_hdr == 1 && g_context->tmp.send_dynamic > 2)
	{
		packet = PACKET_IR_DYN;
		rohc_debugf(3, "choose packet IR-DYN because %d > 2 dynamic fields changed "
		            "with a single IP header\n", g_context->tmp.send_dynamic);
	}
	else if(nr_of_ip_hdr > 1 && g_context->tmp.send_dynamic > 4)
	{
		packet = PACKET_IR_DYN;
		rohc_debugf(3, "choose packet IR-DYN because %d > 4 dynamic fields changed "
		            "with double IP header\n", g_context->tmp.send_dynamic);
	}
	else if(nr_sn_bits <= 14)
	{
		/* UOR-2* packets can be used only if SN stand on <= 14 bits (6 bits
		 * in base header + 8 bits in extension 3): determine which UOR-2*
		 * packet to choose */

		const int is_ip_v4 = (g_context->ip_flags.version == IPV4);
		const int is_rnd = g_context->ip_flags.info.v4.rnd;
		const size_t nr_ip_id_bits = g_context->tmp.nr_ip_id_bits;

		rohc_debugf(3, "choose one UOR-2-* packet because %zd <= 14 SN "
		            "bits must be transmitted\n", nr_sn_bits);

		if(nr_of_ip_hdr == 1) /* single IP header */
		{
			const bool is_ipv4_non_rnd = (is_ip_v4 && !is_rnd);

			if(!is_ipv4_non_rnd)
			{
				packet = PACKET_UOR_2_RTP;
				rohc_debugf(3, "choose packet UOR-2-RTP because the single IP "
				            "header is not 'IPv4 with non-random IP-ID'\n");
			}
			else if(nr_ip_id_bits > 0 && sdvl_can_length_be_encoded(nr_ts_bits))
			{
				/* a UOR-2-ID packet can only carry 29 bits of TS (with ext 3) */
				packet = PACKET_UOR_2_ID;
				rohc_debugf(3, "choose packet UOR-2-ID because the single IP "
				            "header is IPv4 with non-random IP-ID, %zd > 0 "
				            "bits of IP-ID must be transmitted, and %zd TS "
				            "bits can be SDVL-encoded\n", nr_ip_id_bits,
				            nr_ts_bits);
			}
			else
			{
				packet = PACKET_UOR_2_TS;
				rohc_debugf(3, "choose packet UOR-2-TS because the single IP "
				            "header is IPv4 with non-random IP-ID, and UOR-2 "
				            "/ UOR-2-ID packets do not fit\n");
			}
		}
		else /* double IP headers */
		{
			const int is_ip2_v4 = g_context->ip2_flags.version == IPV4;
			const int is_rnd2 = g_context->ip2_flags.info.v4.rnd;
			const size_t nr_ip_id_bits2 = g_context->tmp.nr_ip_id_bits2;
			const bool is_outer_ipv4_non_rnd = (is_ip_v4 && !is_rnd);
			const bool is_inner_ipv4_non_rnd = (is_ip2_v4 && !is_rnd2);
			unsigned int nr_ipv4_non_rnd;
			unsigned int nr_ipv4_non_rnd_with_bits;

			/* find out if how many IP headers are IPv4 headers with
			 * a non-random IP-ID */
			nr_ipv4_non_rnd = 0;
			nr_ipv4_non_rnd_with_bits = 0;
			if(is_outer_ipv4_non_rnd)
			{
				nr_ipv4_non_rnd++;
				if(nr_ip_id_bits > 0)
				{
					nr_ipv4_non_rnd_with_bits++;
				}
			}
			if(is_inner_ipv4_non_rnd)
			{
				nr_ipv4_non_rnd++;
				if(nr_ip_id_bits2 > 0)
				{
					nr_ipv4_non_rnd_with_bits++;
				}
			}

			if(nr_ipv4_non_rnd == 0)
			{
				packet = PACKET_UOR_2_RTP;
				rohc_debugf(3, "choose packet UOR-2-RTP because neither of "
				            "the 2 IP headers are 'IPv4 with non-random "
				            "IP-ID'\n");
			}
			else if(nr_ipv4_non_rnd_with_bits <= 1 &&
			        sdvl_can_length_be_encoded(nr_ts_bits))
			/* TODO: create a is_packet_UOR_2_ID() function */
			{
				packet = PACKET_UOR_2_ID;
				rohc_debugf(3, "choose packet UOR-2-ID because only one of "
				            "the 2 IP headers is IPv4 with non-random IP-ID "
				            "with at least 1 bit of IP-ID to transmit, and "
				            "%zd TS bits can be SDVL-encoded\n", nr_ts_bits);
			}
			else if(nr_ipv4_non_rnd == 1)
			{
				packet = PACKET_UOR_2_TS;
				rohc_debugf(3, "choose packet UOR-2-TS because only one of "
				            "the 2 IP headers is IPv4 with non-random IP-ID\n");
			}
			else
			{
				/* no UO packet fits, use IR-DYN */
				packet = PACKET_IR_DYN;
				rohc_debugf(3, "choose packet IR-DYN because no UO packet fits\n");
			}
		}
	}
	else
	{
		/* UOR-2* packets can not be used, use IR-DYN instead */
		packet = PACKET_IR_DYN;
		rohc_debugf(3, "choose packet IR-DYN because %zd > 14 SN bits must "
		            "be transmitted\n", nr_sn_bits);
	}

	return packet;
}


/**
 * @brief Decide which packet to send when in Second Order (SO) state.
 *
 * Packets that can be used are the UO-0, UO-1 and UO-2 (with or without
 * extensions) packets.
 *
 * @see decide_packet
 *
 * @param context The compression context
 * @return        The packet type among PACKET_UO_0, PACKET_UO_1 and
 *                PACKET_UOR_2
 */
static rohc_packet_t c_rtp_decide_SO_packet(const struct c_context *context)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	int nr_of_ip_hdr;
	size_t nr_sn_bits;
	size_t nr_ts_bits;
	size_t nr_ip_id_bits;
	rohc_packet_t packet;
	int is_rnd;
	int is_ip_v4;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;
	nr_of_ip_hdr = g_context->tmp.nr_of_ip_hdr;
	nr_sn_bits = g_context->tmp.nr_sn_bits;
	nr_ts_bits = rtp_context->tmp.nr_ts_bits;
	nr_ip_id_bits = g_context->tmp.nr_ip_id_bits;
	is_rnd = g_context->ip_flags.info.v4.rnd;
	is_ip_v4 = g_context->ip_flags.version == IPV4;

	rohc_debugf(3, "nr_ip_bits = %zd, nr_sn_bits = %zd, nr_ts_bits = %zd, "
	            "m_set = %d, nr_of_ip_hdr = %d, rnd = %d\n", nr_ip_id_bits,
	            nr_sn_bits, nr_ts_bits, rtp_context->tmp.m_set, nr_of_ip_hdr,
	            is_rnd);

	if(nr_of_ip_hdr == 1) /* single IP header */
	{
		const bool is_ipv4_non_rnd = (is_ip_v4 && !is_rnd);

		if(!is_ipv4_non_rnd && nr_sn_bits <= 4 && nr_ts_bits == 0 &&
		   rtp_context->tmp.m_set == 0)
		{
			packet = PACKET_UO_0;
			rohc_debugf(3, "choose packet UO-0 because the single IP header is "
			            "not 'IPv4 with non-random IP-ID', %zd <= 4 SN bits "
			            "must be transmitted, %s and RTP M bit is not set\n",
			            nr_sn_bits,
			            (nr_ts_bits == 0 ? "0 TS bit must be transmitted" :
			             "TS bits are deductible"));
		}
		else if(!is_ipv4_non_rnd && nr_sn_bits <= 4 && nr_ts_bits <= 6)
		{
			packet = PACKET_UO_1_RTP;
			rohc_debugf(3, "choose packet UO-1-RTP because the single IP "
			            "header is not 'IPv4 with non-random IP-ID', "
			            "%zd <= 4 SN bits and %zd <= 6 TS bits must be "
			            "transmitted\n", nr_sn_bits, nr_ts_bits);
		}
		else if(!is_ipv4_non_rnd)
		{
			packet = PACKET_UOR_2_RTP;
			rohc_debugf(3, "choose packet UOR-2-RTP because the single IP "
			            "header is not 'IPv4 with non-random IP-ID' and "
			            "UO-0 / UO-1-RTP packets do not fit\n");
		}
		else if(nr_sn_bits <= 4 && nr_ip_id_bits == 0 && nr_ts_bits == 0 &&
		        rtp_context->tmp.m_set == 0)
		{
			packet = PACKET_UO_0;
			rohc_debugf(3, "choose packet UO-0 because the single IP header is "
			            "IPv4 with non-random IP-ID, %zd <= 4 SN bits must be "
			            "transmitted, 0 IP-ID bit must be transmitted, %s and "
			            "RTP M bit is not set\n", nr_sn_bits,
			            (nr_ts_bits == 0 ? "0 TS bit must be transmitted" :
			             "TS bits are deductible"));
		}
		else if(nr_sn_bits <= 4 && nr_ip_id_bits == 0 && nr_ts_bits <= 5)
		{
			packet = PACKET_UO_1_TS;
			rohc_debugf(3, "choose packet UO-1-TS because the single IP "
			            "header is IPv4 with non-random IP-ID, %zd <= 4 SN "
			            "bits, 0 IP-ID bit and %zd <= 5 TS bits must be "
			            "transmitted\n", nr_sn_bits, nr_ts_bits);
		}
		else if(nr_sn_bits <= 4 && nr_ip_id_bits <= 5 && nr_ts_bits == 0 &&
		        rtp_context->tmp.m_set == 0)
		{
			/* TODO: when extensions are supported within the UO-1-ID
			 * packet, please check whether the "m_set == 0" condition
			 * could be removed or not */
			packet = PACKET_UO_1_ID;
			rohc_debugf(3, "choose packet UO-1-ID because the single IP header "
			            "is IPv4 with non-random IP-ID, %zd <= 4 SN must be "
			            "transmitted, %zd <= 5 IP-ID bits must be transmitted, "
			            "%s and RTP M bit is not set\n", nr_sn_bits, nr_ip_id_bits,
			            (nr_ts_bits == 0 ? "0 TS bit must be transmitted" :
			             "TS bits are deductible"));
		}
		else if(nr_ip_id_bits > 0 &&
		        sdvl_can_length_be_encoded(nr_ts_bits))
		{
			packet = PACKET_UOR_2_ID;
			rohc_debugf(3, "choose packet UOR-2-ID because the single IP "
			            "header is IPv4 with non-random IP-ID, %zd > 0 IP-ID "
			            "bits must be transmitted, and %zd TS bits can be "
			            "SDVL-encoded\n", nr_ip_id_bits, nr_ts_bits);
		}
		else
		{
			packet = PACKET_UOR_2_TS;
			rohc_debugf(3, "choose packet UOR-2-TS because the single IP "
			            "header is IPv4 with non-random IP-ID and UO-0 / "
			            "UO-1-TS / UO-1-ID / UOR-2-ID packets do not fit\n");
		}
	}
	else /* double IP headers */
	{
		const int is_ip2_v4 = (g_context->ip2_flags.version == IPV4);
		const int is_rnd2 = g_context->ip2_flags.info.v4.rnd;
		const size_t nr_ip_id_bits2 = g_context->tmp.nr_ip_id_bits2;
		const bool is_outer_ipv4_non_rnd = (is_ip_v4 && !is_rnd);
		const bool is_inner_ipv4_non_rnd = (is_ip2_v4 && !is_rnd2);
		unsigned int nr_ipv4_non_rnd;
		unsigned int nr_ipv4_non_rnd_with_bits;

		/* find out if how many IP headers are IPv4 headers with
		 * a non-random IP-ID */
		nr_ipv4_non_rnd = 0;
		nr_ipv4_non_rnd_with_bits = 0;
		if(is_outer_ipv4_non_rnd)
		{
			nr_ipv4_non_rnd++;
			if(nr_ip_id_bits > 0)
			{
				nr_ipv4_non_rnd_with_bits++;
			}
		}
		if(is_inner_ipv4_non_rnd)
		{
			nr_ipv4_non_rnd++;
			if(nr_ip_id_bits2 > 0)
			{
				nr_ipv4_non_rnd_with_bits++;
			}
		}
		rohc_debugf(3, "nr_ipv4_non_rnd = %u, nr_ipv4_non_rnd_with_bits = %u\n",
		            nr_ipv4_non_rnd, nr_ipv4_non_rnd_with_bits);

		if(nr_sn_bits <= 4 && nr_ipv4_non_rnd_with_bits == 0 &&
		   nr_ts_bits == 0 && rtp_context->tmp.m_set == 0)
		{
			packet = PACKET_UO_0;
			rohc_debugf(3, "choose packet UO-0 because %zd <= 4 SN bits must be "
			            "transmitted, neither of the 2 IP headers are IPv4 with "
			            "non-random IP-ID with some IP-ID bits to transmit, %s, "
			            "and RTP M bit is not set\n", nr_sn_bits,
			            (nr_ts_bits == 0 ? "0 TS bit must be transmitted" :
			             "TS bits are deductible"));
		}
		else if(nr_ipv4_non_rnd == 0 && nr_sn_bits <= 4 && nr_ts_bits <= 6)
		{
			packet = PACKET_UO_1_RTP;
			rohc_debugf(3, "choose packet UO-1-RTP because neither of the 2 "
			            "IP headers are 'IPv4 with non-random IP-ID', "
			            "%zd <= 4 SN bits must be transmitted, %zd <= 6 TS "
			            "bits must be transmitted\n", nr_sn_bits, nr_ts_bits);
		}
		else if(nr_ipv4_non_rnd_with_bits <= 1 &&
		        (nr_ip_id_bits <= 5 || nr_ip_id_bits2 <= 5) &&
		        nr_sn_bits <= 4 && nr_ts_bits == 0 && rtp_context->tmp.m_set == 0)
		{
			/* TODO: when extensions are supported within the UO-1-ID packet,
			 * please check whether the "m_set == 0" condition could be
			 * removed or not */
			packet = PACKET_UO_1_ID;
			rohc_debugf(3, "choose packet UO-1-ID because only one of the 2 "
			            "IP headers is IPv4 with non-random IP-ID with %zd "
			            "<= 5 IP-ID bits to transmit, %zd <= 4 SN bits must "
			            "be transmitted, %s, and RTP M bit is not set\n",
			            rohc_max(nr_ip_id_bits, nr_ip_id_bits2), nr_sn_bits,
			            (nr_ts_bits == 0 ? "0 TS bit must be transmitted" :
			             "TS bits are deductible"));
		}
		else if(nr_ipv4_non_rnd_with_bits == 0 &&
		        nr_sn_bits <= 4 &&
		        nr_ts_bits <= 5)
		{
			packet = PACKET_UO_1_TS;
			rohc_debugf(3, "choose packet UO-1-TS because neither of the 2 "
			            "IP headers are IPv4 with non-random IP-ID with some "
			            "IP-ID bits to to transmit for that IP header, "
			            "%zd <= 4 SN bits must be transmitted, %zd <= 6 TS "
			            "bits must be transmitted\n", nr_sn_bits, nr_ts_bits);
		}
		else if(nr_ipv4_non_rnd == 0)
		{
			packet = PACKET_UOR_2_RTP;
			rohc_debugf(3, "choose packet UOR-2-RTP because neither of the 2 "
			            "IP headers are 'IPv4 with non-random IP-ID'\n");
		}
		else if(nr_ipv4_non_rnd_with_bits <= 1 &&
		        sdvl_can_length_be_encoded(nr_ts_bits))
		/* TODO: create a is_packet_UOR_2_ID() function */
		{
			packet = PACKET_UOR_2_ID;
			rohc_debugf(3, "choose packet UOR-2-ID because only one of "
			            "the 2 IP headers is IPv4 with non-random IP-ID "
			            "with at least 1 bit of IP-ID to transmit, and "
			            "%zd TS bits can be SDVL-encoded\n", nr_ts_bits);
		}
		else if(nr_ipv4_non_rnd == 1)
		{
			packet = PACKET_UOR_2_TS;
			rohc_debugf(3, "choose packet UOR-2-TS because only one of "
			            "the 2 IP headers is IPv4 with non-random IP-ID\n");
		}
		else
		{
			/* no UO packet fits, use IR-DYN */
			packet = PACKET_IR_DYN;
			rohc_debugf(3, "choose packet IR-DYN because no UO packet fits\n");
		}
	}

	return packet;
}


/**
 * @brief Decide what extension shall be used in the UO-1/UO-2 packet.
 *
 * Extensions 0, 1 & 2 are IPv4 only because of the IP-ID.
 *
 * @param context The compression context
 * @return        The extension code among PACKET_NOEXT, PACKET_EXT_0,
 *                PACKET_EXT_1 and PACKET_EXT_3 if successful,
 *                PACKET_EXT_UNKNOWN otherwise
 */
static rohc_ext_t c_rtp_decide_extension(const struct c_context *context)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	rohc_ext_t ext;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;

	/* force extension type 3 if at least one RTP dynamic field changed */
	if(rtp_context->tmp.send_rtp_dynamic > 0)
	{
		rohc_debugf(3, "force EXT-3 because at least one RTP dynamic "
		            "field changed\n");
		ext = PACKET_EXT_3;
	}
	else
	{
		/* fallback on the algorithm shared by all IP-based profiles */
		ext = decide_extension(context);
	}

	return ext;
}


/**
 * @brief Encode an IP/UDP/RTP packet according to a pattern decided by several
 *        different factors.
 *
 * @param context        The compression context
 * @param ip             The IP packet to encode
 * @param packet_size    The length of the IP packet to encode
 * @param dest           The rohc-packet-under-build buffer
 * @param dest_size      The length of the rohc-packet-under-build buffer
 * @param packet_type    OUT: The type of ROHC packet that is created
 * @param payload_offset The offset for the payload in the IP packet
 * @return               The length of the created ROHC packet
 */
int c_rtp_encode(struct c_context *const context,
                 const struct ip_packet *ip,
                 const int packet_size,
                 unsigned char *const dest,
                 const int dest_size,
                 rohc_packet_t *const packet_type,
                 int *const payload_offset)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	struct ip_packet ip2;
	const struct ip_packet *last_ip_header;
	const struct udphdr *udp;
	const struct rtphdr *rtp;
	unsigned int ip_proto;
	int size;

	g_context = (struct c_generic_context *) context->specific;
	if(g_context == NULL)
	{
		rohc_debugf(0, "generic context not valid\n");
		return -1;
	}

	rtp_context = (struct sc_rtp_context *) g_context->specific;
	if(rtp_context == NULL)
	{
		rohc_debugf(0, "RTP context not valid\n");
		return -1;
	}

	ip_proto = ip_get_protocol(ip);
	if(ip_proto == IPPROTO_IPIP || ip_proto == IPPROTO_IPV6)
	{
		/* get the last IP header */
		if(!ip_get_inner_packet(ip, &ip2))
		{
			rohc_debugf(0, "cannot create the inner IP header\n");
			return -1;
		}
		last_ip_header = &ip2;

		/* get the transport protocol */
		ip_proto = ip_get_protocol(last_ip_header);
	}
	else
	{
		/* only one single IP header, the last IP header is the first one */
		last_ip_header = ip;
	}

	if(ip_proto != IPPROTO_UDP)
	{
		rohc_debugf(0, "packet is not an UDP packet\n");
		return -1;
	}
	udp = (struct udphdr *) ip_get_next_layer(last_ip_header);
	rtp = (struct rtphdr *) (udp + 1);

	/* how many UDP/RTP fields changed? */
	rtp_context->tmp.send_rtp_dynamic = rtp_changed_rtp_dynamic(context, udp);

	/* encode the IP packet */
	size = c_generic_encode(context, ip, packet_size, dest, dest_size,
	                        packet_type, payload_offset);
	if(size < 0)
	{
		goto quit;
	}

	/* update the context with the new UDP/RTP headers */
	if(g_context->tmp.packet_type == PACKET_IR ||
	   g_context->tmp.packet_type == PACKET_IR_DYN)
	{
		rtp_context->old_udp = *udp;
		rtp_context->old_rtp = *rtp;
	}

quit:
	return size;
}


/**
 * @brief Decide the state that should be used for the next packet compressed
 *        with the ROHC RTP profile.
 *
 * The three states are:
 *  - Initialization and Refresh (IR),
 *  - First Order (FO),
 *  - Second Order (SO).
 *
 * @param context The compression context
 */
void rtp_decide_state(struct c_context *const context)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;

	if(rtp_context->ts_sc.state == INIT_TS)
	{
		change_state(context, IR);
	}
	else if(context->state == IR &&
	        rtp_context->ts_sc.state == INIT_STRIDE &&
	        is_ts_constant(rtp_context->ts_sc))
	{
		/* init ts_stride but timestamp is constant so we stay in IR */
		rohc_debugf(3, "init ts_stride but timestamp is constant -> stay in IR\n");
		change_state(context, IR);
	}
	else if(rtp_context->udp_checksum_change_count < MAX_IR_COUNT)
	{
		/* TODO: could be optimized: IR state is not required, only IR or
		 * IR-DYN packet is */
		rohc_debugf(3, "go back to IR state because UDP checksum behaviour "
		            "changed in the last few packets\n");
		change_state(context, IR);
	}
	else if(rtp_context->ts_sc.state == INIT_STRIDE &&
	        context->state != IR &&
	        is_ts_constant(rtp_context->ts_sc))
	{
		/* init ts_stride but timestamp is contant -> FO */
		rohc_debugf(3, "init ts_stride but timestamp is constant -> FO\n");
		change_state(context, FO);
	}
	else if(rtp_context->tmp.send_rtp_dynamic && context->state != IR)
	{
		rohc_debugf(3, "send_rtp_dynamic != 0 -> FO\n");
		change_state(context, FO);
	}
	else
	{
		/* generic function used by the IP-only, UDP and UDP-Lite profiles */
		decide_state(context);
	}
}


/**
 * @brief Determine the SN value for the next packet
 *
 * Profile SN is the RTP SN.
 *
 * @param context   The compression context
 * @param outer_ip  The outer IP header
 * @param inner_ip  The inner IP header if it exists, NULL otherwise
 * @return          The SN
 */
static uint16_t c_rtp_get_next_sn(const struct c_context *context,
                                  const struct ip_packet *outer_ip,
                                  const struct ip_packet *inner_ip)
{
	struct c_generic_context *g_context;
	struct udphdr *udp;
	struct rtphdr *rtp;
	uint16_t next_sn;

	g_context = (struct c_generic_context *) context->specific;

	/* get UDP and RTP headers */
	if(g_context->tmp.nr_of_ip_hdr > 1)
	{
		udp = (struct udphdr *) ip_get_next_layer(inner_ip);
	}
	else
	{
		udp = (struct udphdr *) ip_get_next_layer(outer_ip);
	}
	rtp = (struct rtphdr *) (udp + 1);

	next_sn = ntohs(rtp->sn);

	return next_sn;
}


/**
 * @brief Build the static part of the UDP/RTP headers.
 *
 * \verbatim

 Static part of UDP header (5.7.7.5):

    +---+---+---+---+---+---+---+---+
 1  /          Source Port          /   2 octets
    +---+---+---+---+---+---+---+---+
 2  /       Destination Port        /   2 octets
    +---+---+---+---+---+---+---+---+

 Static part of RTP header (5.7.7.6):

    +---+---+---+---+---+---+---+---+
 3  /             SSRC              /   4 octets
    +---+---+---+---+---+---+---+---+

\endverbatim
 *
 * Parts 1 & 2 are done by the udp_code_static_udp_part() function. Part 3 is
 * done by this function.
 *
 * @param context     The compression context
 * @param next_header The UDP/RTP headers
 * @param dest        The rohc-packet-under-build buffer
 * @param counter     The current position in the rohc-packet-under-build buffer
 * @return            The new position in the rohc-packet-under-build buffer
 *
 * @see udp_code_static_udp_part
 */
int rtp_code_static_rtp_part(const struct c_context *context,
                             const unsigned char *next_header,
                             unsigned char *const dest,
                             int counter)
{
	struct udphdr *udp = (struct udphdr *) next_header;
	struct rtphdr *rtp = (struct rtphdr *) (udp + 1);

	/* parts 1 & 2 */
	counter = udp_code_static_udp_part(context, next_header, dest, counter);

	/* part 3 */
	rohc_debugf(3, "RTP SSRC = 0x%x\n", rtp->ssrc);
	memcpy(&dest[counter], &rtp->ssrc, 4);
	counter += 4;

	return counter;
}


/**
 * @brief Build the dynamic part of the UDP/RTP headers.
 *
 * \verbatim

 Dynamic part of UDP header (5.7.7.5):

    +---+---+---+---+---+---+---+---+
 1  /           Checksum            /   2 octets
    +---+---+---+---+---+---+---+---+

 Dynamic part of RTP header (5.7.7.6):

    +---+---+---+---+---+---+---+---+
 2  |  V=2  | P | RX|      CC       |  (RX is NOT the RTP X bit)
    +---+---+---+---+---+---+---+---+
 3  | M |            PT             |
    +---+---+---+---+---+---+---+---+
 4  /      RTP Sequence Number      /  2 octets
    +---+---+---+---+---+---+---+---+
 5  /   RTP Timestamp (absolute)    /  4 octets
    +---+---+---+---+---+---+---+---+
 6  /      Generic CSRC list        /  variable length
    +---+---+---+---+---+---+---+---+
 7  : Reserved  | X |  Mode |TIS|TSS:  if RX = 1
    +---+---+---+---+---+---+---+---+
 8  :         TS_Stride             :  1-4 octets, if TSS = 1
    +---+---+---+---+---+---+---+---+
 9  :         Time_Stride           :  1-4 octets, if TIS = 1
    +---+---+---+---+---+---+---+---+

\endverbatim
 *
 * Parts 6 & 9 are not supported yet. The TIS flag in part 7 is not supported.
 *
 * @param context     The compression context
 * @param next_header The UDP/RTP headers
 * @param dest        The rohc-packet-under-build buffer
 * @param counter     The current position in the rohc-packet-under-build buffer
 * @return            The new position in the rohc-packet-under-build buffer
 */
int rtp_code_dynamic_rtp_part(const struct c_context *context,
                              const unsigned char *next_header,
                              unsigned char *const dest,
                              int counter)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	struct udphdr *udp;
	struct rtphdr *rtp;
	unsigned char byte;
	unsigned int rx_byte = 0;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;

	udp = (struct udphdr *) next_header;
	rtp = (struct rtphdr *) (udp + 1);

	/* part 1 */
	rohc_debugf(3, "UDP checksum = 0x%04x\n", udp->check);
	memcpy(&dest[counter], &udp->check, 2);
	counter += 2;
	rtp_context->udp_checksum_change_count++;

	/* part 2 */
	byte = 0;
	if(!is_ts_constant(rtp_context->ts_sc) &&
	   (rtp_context->ts_sc.state == INIT_STRIDE ||
	    (g_context->tmp.packet_type == PACKET_IR &&
	     rtp_context->ts_sc.state == SEND_SCALED)))
	{
		/* send ts_stride */
		rx_byte = 1;
		byte |= 1 << 4;
	}
	byte |= (rtp->version & 0x03) << 6;
	byte |= (rtp->padding & 0x01) << 5;
	byte |= rtp->cc & 0x0f;
	dest[counter] = byte;
	rohc_debugf(3, "part 2 = 0x%02x\n", dest[counter]);
	counter++;

	/* part 3 */
	byte = 0;
	byte |= (rtp->m & 0x01) << 7;
	byte |= rtp->pt & 0x7f;
	dest[counter] = byte;
	rohc_debugf(3, "part 3 = 0x%02x\n", dest[counter]);
	counter++;
	rtp_context->rtp_pt_change_count++;

	/* part 4 */
	memcpy(&dest[counter], &rtp->sn, 2);
	rohc_debugf(3, "part 4 = 0x%02x 0x%02x\n", dest[counter], dest[counter + 1]);
	counter += 2;

	/* part 5 */
	memcpy(&dest[counter], &rtp->timestamp, 4);
	rohc_debugf(3, "part 5 = 0x%02x 0x%02x 0x%02x 0x%02x\n", dest[counter],
	            dest[counter + 1], dest[counter + 2], dest[counter + 3]);
	counter += 4;

	/* part 6 not supported yet  but the field is mandatory,
	   so add a zero byte */
	dest[counter] = 0x00;
	rohc_debugf(3, "Generic CSRC list not supported yet, put a 0x00 byte\n");
	counter++;

	/* parts 7, 8 & 9 */
	if(rx_byte)
	{
		int tis;
		int tss;

		/* part 7 */
		tis = 0; /* TIS flag not supported yet */
		tss = rtp_context->ts_sc.state != INIT_TS ? 1 : 0;

		byte = 0;
		byte |= (rtp->extension & 0x01) << 4;
		byte |= (context->mode & 0x03) << 2;
		byte |= (tis & 0x01) << 1;
		byte |= tss & 0x01;
		dest[counter] = byte;
		rohc_debugf(3, "part 7 = 0x%02x\n", dest[counter]);
		counter++;

		/* part 8 */
		if(tss)
		{
			uint32_t ts_stride;
			size_t ts_stride_sdvl_len;
			int ret;

			/* get the TS_STRIDE to send in packet */
			ts_stride = get_ts_stride(rtp_context->ts_sc);

			/* how many bytes are required by SDVL to encode TS_STRIDE ? */
			ts_stride_sdvl_len = c_bytesSdvl(ts_stride, 0 /* length detection */);
			if(ts_stride_sdvl_len <= 0 || ts_stride_sdvl_len > 4)
			{
				rohc_debugf(0, "failed to determine the number of bits required to "
				            "SDVL-encode TS_STRIDE %u (%zd)\n", ts_stride,
				            ts_stride_sdvl_len);
				/* TODO: should handle error gracefully */
				assert(0);
			}

			rohc_debugf(3, "send ts_stride = 0x%08x encoded with SDVL on %zd bytes\n",
			            ts_stride, ts_stride_sdvl_len);

			/* encode TS_STRIDE in SDVL and write it to packet */
			ret = c_encodeSdvl(&dest[counter], ts_stride, 0 /* length detection */);
			if(ret != 1)
			{
				rohc_debugf(0, "failed to SDVL-encode TS_STRIDE %u\n", ts_stride);
				/* TODO: should handle error gracefully */
				assert(0);
			}

			/* skip the bytes used to encode TS_STRIDE in SDVL */
			counter += ts_stride_sdvl_len;

			/* do we transmit the scaled RTP Timestamp (TS) in the next packet ? */
			if(rtp_context->ts_sc.state == INIT_STRIDE)
			{
				rtp_context->ts_sc.nr_init_stride_packets++;
				if(rtp_context->ts_sc.nr_init_stride_packets >= ROHC_INIT_TS_STRIDE_MIN)
				{
					rohc_debugf(3, "TS_STRIDE transmitted at least %u times, so change "
					            "from state INIT_STRIDE to SEND_SCALED\n",
					            ROHC_INIT_TS_STRIDE_MIN);
					rtp_context->ts_sc.state = SEND_SCALED;
				}
				else
				{
					rohc_debugf(3, "TS_STRIDE transmitted only %zd times, so stay in "
					            "state INIT_STRIDE (at least %u times are required "
					            "to change to state SEND_SCALED)\n",
					            rtp_context->ts_sc.nr_init_stride_packets,
					            ROHC_INIT_TS_STRIDE_MIN);
				}
			}
		}

		/* part 9 not supported yet */
	}

	if(rtp_context->ts_sc.state == INIT_TS)
	{
		rohc_debugf(3, "change from state INIT_TS to INIT_STRIDE\n");
		rtp_context->ts_sc.state = INIT_STRIDE;
		rtp_context->ts_sc.nr_init_stride_packets = 0;
	}

	return counter;
}


/**
 * @brief Check if the dynamic part of the UDP/RTP headers changed.
 *
 * @param context The compression context
 * @param udp     The UDP/RTP headers
 * @return        The number of UDP/RTP fields that changed
 */
int rtp_changed_rtp_dynamic(const struct c_context *context,
                            const struct udphdr *udp)
{
	struct c_generic_context *g_context;
	struct sc_rtp_context *rtp_context;
	struct rtphdr *rtp;
	int fields = 0;

	g_context = (struct c_generic_context *) context->specific;
	rtp_context = (struct sc_rtp_context *) g_context->specific;

	rtp = (struct rtphdr *) (udp + 1);

	rohc_debugf(2, "find changes in RTP dynamic fields\n");

	/* check UDP checksum field */
	if((udp->check != 0 && rtp_context->old_udp.check == 0) ||
	   (udp->check == 0 && rtp_context->old_udp.check != 0) ||
	   (rtp_context->udp_checksum_change_count < MAX_IR_COUNT))
	{
		if((udp->check != 0 && rtp_context->old_udp.check == 0) ||
		   (udp->check == 0 && rtp_context->old_udp.check != 0))
		{
			rohc_debugf(3, "UDP checksum field changed\n");
			rtp_context->udp_checksum_change_count = 0;
		}
		else
		{
			rohc_debugf(3, "UDP checksum field did not change "
			            "but changed in the last few packets\n");
		}

		/* do not count the UDP checksum change as other RTP dynamic fields
		 * because it requires a specific behaviour (IR or IR-DYN packet
		 * required). */
	}

	/* check RTP CSRC Counter and CSRC field */
	if(rtp->cc != rtp_context->old_rtp.cc)
	{
		rohc_debugf(3, "RTP CC field changed (0x%x -> 0x%x)\n",
		            rtp_context->old_rtp.cc, rtp->cc);
		fields += 2;
	}

	/* check SSRC field */
	if(rtp->ssrc != rtp_context->old_rtp.ssrc)
	{
		rohc_debugf(3, "RTP SSRC field changed (0x%08x -> 0x%08x)\n",
		            rtp_context->old_rtp.ssrc, rtp->ssrc);
		fields++;
	}

	/* check RTP Marker field: remember its value but do not count it
	 * as a changed field since it is not stored in the context */
	if(rtp->m != 0)
	{
		rohc_debugf(3, "RTP Marker (M) bit is set\n");
		rtp_context->tmp.m_set = 1;
	}
	else
	{
		rtp_context->tmp.m_set = 0;
	}

	/* check RTP Payload Type field */
	if(rtp->pt != rtp_context->old_rtp.pt ||
	   rtp_context->rtp_pt_change_count < MAX_IR_COUNT)
	{
		if(rtp->pt != rtp_context->old_rtp.pt)
		{
			rohc_debugf(3, "RTP Payload Type (PT) field changed (0x%x -> 0x%x)\n",
			            rtp_context->old_rtp.pt, rtp->pt);
			rtp_context->tmp.rtp_pt_changed = 1;
			rtp_context->rtp_pt_change_count = 0;
		}
		else
		{
			rohc_debugf(3, "RTP Payload Type (PT) field did not change "
			            "but changed in the last few packets\n");
		}

		fields++;
	}
	else
	{
		rtp_context->tmp.rtp_pt_changed = 0;
	}

	/* we verify if ts_stride changed */
	rtp_context->tmp.timestamp = ntohl(rtp->timestamp);

	rohc_debugf(2, "%d RTP dynamic fields changed\n", fields);

	return fields;
}


/// List of UDP ports which are associated with RTP streams
int rtp_ports[] = { RTP_PORTS, 0 };


/**
 * @brief Define the compression part of the RTP profile as described
 *        in the RFC 3095.
 */
struct c_profile c_rtp_profile =
{
	IPPROTO_UDP,         /* IP protocol */
	rtp_ports,           /* list of UDP ports */
	ROHC_PROFILE_RTP,    /* profile ID */
	"RTP / Compressor",  /* profile description */
	c_rtp_create,        /* profile handlers */
	c_rtp_destroy,
	c_rtp_check_context,
	c_rtp_encode,
	c_generic_feedback,
};

