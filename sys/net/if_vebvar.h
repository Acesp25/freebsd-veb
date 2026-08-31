/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright 2026 Aaron Espinoza <acesp25@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _NET_IF_VEBVAR_H_
#define _NET_IF_VEBVAR_H_

#include <sys/types.h>

#include <net/ethernet.h>
#include <net/if.h>

#define VEBADD		0
#define VEBDEL		1
#define VEBGIFS		2	/* get member list (ifvconf) */
#define VEBGRTS		3	/* get address list (ifvaconf) */
#define	VEBGIFFLGS	4	/* get member if flags (ifvebreq) */
#define	VEBSIFFLGS	5	/* set member if flags (ifvebreq) */
#define	VEBGTO		6	/* get cache timeout (ifvrparam) */
#define	VEBSTO		7	/* set cache timeout (ifvrparam) */

typedef uint32_t ifveb_flags_t;

#define IFVEB_HASVPORT	1	/* veb interface contains vport member */
#define	IFVEBBITS	"\020\01HASVPORT"

/*
 * Generic veb control request.
 */
struct ifvreq {
	char		ifvr_ifsname[IFNAMSIZ];
	uint32_t	ifvr_ifsflags;		/* current flags (get) */
	uint32_t	ifvr_ifssetmask;	/* bits to set (set) */
	uint32_t	ifvr_ifsclrmask;	/* bits to clear (set) */
};

/* VEBFLUSH */
#define IFVF_FLUSHDYN	0x00	/* flush learned addresses only */
#define IFVF_FLUSHALL	0x01	/* flush all addresses */

#define IFVP_LEARNING	0x0001	/* if can learn */
#define IFVP_DISCOVER	0x0002	/* if sends packets w/ unknown dest. */
#define IFVP_STICKY	0x0004	/* if learned addresses stick */
#define IFVP_PRIVATE	0x0008	/* if is a private segment */
#define IFVP_VPORT	0x0010	/* if is vport */

#define IFVPBITS "\020\1LEARNING\2DISCOVER\3STICKY\4PRIVATE\5VPORT"
#define IFVPMASK  (IFVP_LEARNING|IFVP_DISCOVER|IFVP_STICKY|IFVP_PRIVATE|IFVP_VPORT)
#define IFVPUMASK (IFVPMASK & ~IFVP_VPORT)  /* user-settable subset */

/*
 * Interface list structure.
 */
struct ifvpconf {
	uint32_t ifvpc_len; /* buffer size */
	union {
		caddr_t ifvpcu_buf;
		struct ifvreq *ifvpcu_req;
	} ifvpc_ifvpcu;
#define ifvpc_buf ifvpc_ifvpcu.ifvpcu_buf
#define ifvpc_req ifvpc_ifvpcu.ifvpcu_req
};

#define IFVAF_TYPEMASK	0x03	/* address type mask */
#define IFVAF_DYNAMIC		0x00	/* dynamically learned address */
#define IFVAF_STATIC		0x01	/* static address */
#define IFVAF_STICKY		0x02	/* sticky address */

#define IFVAFBITS     "\020\1STATIC\2STICKY"

/*
 * Veb address request.
 */
struct ifvareq {
	char ifva_ifsname[IFNAMSIZ];	  /* member if name */
	unsigned long ifva_expire;	  /* address expire time */
	uint8_t ifva_flags;		  /* address flags */
	uint8_t ifva_dst[ETHER_ADDR_LEN]; /* destination address */
	ether_vlanid_t ifva_vlan;	  /* vlan id */
};

/*
 * Address list structure.
 */
struct ifvaconf {
	uint32_t ifvac_len;	/* buffer size */
	union {
		caddr_t ifvacu_buf;
		struct ifvareq *ifvacu_req;
	} ifvac_ifvacu;
#define ifvac_buf ifvac_ifvacu.ifvacu_buf
#define ifvac_req ifvac_ifvacu.ifvacu_req
};

/*
 * Veb parameter structure.
 */
struct ifvrparam {
	union {
		uint32_t ifvrpu_int32;
		uint16_t ifvrpu_int16;
		uint8_t ifvrpu_int8;
	} ifvrp_ifvrpu;
};
#define	ifvrp_ctime	ifvrp_ifvrpu.ifvrpu_int32	/* cache time (sec) */

#endif /* _NET_IF_VEBVAR_H_ */
