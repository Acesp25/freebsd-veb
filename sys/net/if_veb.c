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

#define EXTERR_CATEGORY EXTERR_CAT_BRIDGE

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/exterrvar.h>
#include <sys/eventhandler.h>
#include <sys/callout.h>
#include <sys/cdefs.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/systm.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/if_var.h>
#include <net/if_vlan_var.h>
#include <net/if_private.h>
#include <net/if_vlan_var.h>
#include <net/vnet.h>

#include <net/if_vebvar.h>


/*
 * Size of the route hash table.  Must be a power of two.
 */
#ifndef VEB_RTHASH_SIZE
#define	VEB_RTHASH_SIZE		1024
#endif

#define	VEB_RTHASH_MASK		(VEB_RTHASH_SIZE - 1)

/*
 * Default maximum number of addresses to cache.
 */
#ifndef VEB_RTABLE_MAX
#define	VEB_RTABLE_MAX		2000
#endif

/*
 * Timeout (in seconds) for entries learned dynamically.
 */
#ifndef VEB_RTABLE_TIMEOUT
#define	VEB_RTABLE_TIMEOUT	(20 * 60)	/* same as ARP */
#endif

/*
 * Number of seconds between walks of the route list.
 */
#ifndef VEB_RTABLE_PRUNE_PERIOD
#define	VEB_RTABLE_PRUNE_PERIOD	(5 * 60)
#endif

/*
 * List of capabilities to possibly mask on the member interface.
 */
#define VEB_IFCAPS_MASK		(IFCAP_TOE|IFCAP_TSO|IFCAP_TXCSUM|\
				IFCAP_TXCSUM_IPV6|IFCAP_MEXTPG)

/*
 * List of capabilities to strip
 */
#define	VEB_IFCAPS_STRIP	IFCAP_LRO

static const char veb_name[] = "veb";
static MALLOC_DEFINE(M_VEB, "veb", "Virtual Ethernet Bridge psudeo driver");

VNET_DEFINE_STATIC(struct if_clone *, veb_cloner);
#define V_veb_cloner	VNET(veb_cloner)

/*
 * Veb port (member)
 */
struct veb_port {
	CK_LIST_ENTRY(veb_port) vp_next;
	struct ifnet            *vp_ifp;
	struct veb_softc        *vp_sc;
	uint32_t		vp_flags;
	int			vp_savedcaps;	/* saved capabilities */
	uint32_t		vp_addrmax;	/* max # of addresses */
	uint32_t		vp_addrcnt;	/* cur. # of addresses */
	uint32_t		vp_addrexceeded;/* # of address violations */
	struct epoch_context	vp_epoch_ctx;
	int (*vp_ioctl)(struct ifnet *, u_long, caddr_t);
};
#define IS_VPORT(_vp)		((_vp)->vp_flags & IFVP_VPORT)

/*
 * Veb route node.
 */
struct veb_rtnode {
	CK_LIST_ENTRY(veb_rtnode) vrt_hash;	/* hash table linkage */
	CK_LIST_ENTRY(veb_rtnode) vrt_list;	/* list linkage */
	struct veb_port		*vrt_dst;	/* destination if */
	unsigned long		vrt_expire;	/* expiration time */
	uint8_t			vrt_flags;	/* address flags */
	uint8_t			vrt_addr[ETHER_ADDR_LEN];
	struct vnet		*vrt_vnet;
	ether_vlanid_t		vrt_vlan;	/* vlan id */
	struct epoch_context	vrt_epoch_ctx;
};
#define	vrt_ifp			vrt_dst->vp_ifp

struct vport_softc {
	struct ifnet		*sc_ifp;
	struct sx		sc_sx;
	struct veb_port 	*sc_vp;         /* Back pointer to vport's veb_port */
	struct ether_addr	sc_defaddr;	/* Default MAC address */
	struct epoch_context	sc_epoch_ctx;
};

struct veb_softc {
	struct ifnet		*sc_ifp;
	struct sx		sc_sx;
	struct mtx		sc_rt_mtx;
	uint32_t		sc_vrtmax;	/* max # of addresses */
	uint32_t		sc_vrtcnt;	/* cur. # of addresses */
	uint32_t		sc_vrttimeout;	/* rt timeout in seconds */
	struct callout		sc_vebcallout;	/* veb callout */
	struct vport_softc      *sc_vport;      /* pointer to our vport sc */
	CK_LIST_HEAD(, veb_port) sc_vebport;	/* member interface list */
	CK_LIST_HEAD(, veb_rtnode) *sc_rthash;	/* our forwarding table */
	CK_LIST_HEAD(, veb_rtnode) sc_rtlist;	/* list version of above */
	uint32_t		sc_rthash_key;	/* key for hash */
	uint32_t		sc_vrtexceeded;	/* # of cache drops */
	struct ifnet		*sc_ifaddr;	/* member mac copied from */
	struct ether_addr	sc_defaddr;	/* Default MAC address */
	struct epoch_context	sc_epoch_ctx;
	ether_vlanid_t		sc_defpvid;	/* default PVID */
	ifveb_flags_t		sc_flags;	/* veb flags */
};

static const char vport_name[] = "vport";
static MALLOC_DEFINE(M_VPORT, "vport", "Vport network pseudo driver");

VNET_DEFINE_STATIC(struct if_clone *, vport_cloner);
#define V_vport_cloner	VNET(vport_cloner)

/*
 * VEB LOCKING
 *
 * if_veb using the same locking mechanism as if_bridge.c,
 * That being a mtx for routing table operations (non-sleepable) and a sc lock (sleepable)
 * for all other operations.
*/
#define VEB_LOCK_INIT(_sc)	do {				\
	sx_init(&(_sc)->sc_sx, "if_veb");			\
	mtx_init(&(_sc)->sc_rt_mtx, "if_veb rt", NULL, MTX_DEF);\
} while (0)
#define VEB_LOCK_DESTROY(_sc)	do {		\
	sx_destroy(&(_sc)->sc_sx);		\
	mtx_destroy(&(_sc)->sc_rt_mtx);		\
} while (0)
#define VEB_LOCK(_sc)		sx_xlock(&(_sc)->sc_sx)
#define VEB_UNLOCK(_sc)		sx_xunlock(&(_sc)->sc_sx)
#define VEB_ASSERT(_sc)		sx_assert(&(_sc)->sc_sx, SX_XLOCKED)
#define VEB_LOCK_OR_NET_EPOCH_ASSERT(_sc)	\
	    MPASS(in_epoch(net_epoch_preempt) || sx_xlocked(&(_sc)->sc_sx))
#define VEB_UNLOCK_ASSERT(_sc)	sx_assert(&(_sc)->sc_sx, SX_UNLOCKED)
#define VEB_RT_LOCK(_sc)		mtx_lock(&(_sc)->sc_rt_mtx)
#define VEB_RT_UNLOCK(_sc)		mtx_unlock(&(_sc)->sc_rt_mtx)
#define VEB_RT_LOCK_ASSERT(_sc)	mtx_assert(&(_sc)->sc_rt_mtx, MA_OWNED)
#define VEB_RT_LOCK_OR_NET_EPOCH_ASSERT(_sc)	\
	    MPASS(in_epoch(net_epoch_preempt) || mtx_owned(&(_sc)->sc_rt_mtx))


/*
 * VPORT LOCKING
 *
 * A vport softc has a single sx lock covering all of its own state.
 *
 * Lock order across the driver is:
 *
 *	VEB_LOCK -> VPORT_LOCK		veb_ioctl() SIOCSIFMTU pushes the
 *					MTU down through vport_ioctl(), and
 *					veb_set_ifcap() reaches a vport the
 *					same way.
 *	VEB_LOCK -> VEB_RT_LOCK		veb_delete_member(), rt flush paths.
 *
 * Neither reverse order is taken anywhere. vport_ioctl() must therefore
 * never acquire VEB_LOCK; it only ever tests sc_vp for NULL.
 *
 * vport_ioctl() drops VPORT_LOCK around ether_ioctl() because the
 * SIOCSIFADDR path calls back into vport_init(), which takes it.
 *
 * vport_softc.sc_vp is not covered by VPORT_LOCK. It is written under
 * VEB_LOCK by veb_ioctl_add() and veb_delete_member(), and read under
 * NET_EPOCH by vport_transmit() and veb_port_of(), which is what keeps the
 * veb_port alive across the read. vport_ioctl() reads it while holding
 * only VPORT_LOCK; that read is unsynchronised against the VEB_LOCK
 * writer, but it is used purely for policy decisions (may this MTU change
 * proceed, may this address be assigned) where losing a race just means
 * the caller sees the answer from a moment earlier.
 */
#define VPORT_LOCK_INIT(_sc)	do {				\
	sx_init(&(_sc)->sc_sx, "if_vport");			\
} while (0)
#define VPORT_LOCK_DESTROY(_sc)	do {		\
	sx_destroy(&(_sc)->sc_sx);		\
} while (0)
#define VPORT_LOCK(_sc)		sx_xlock(&(_sc)->sc_sx)
#define VPORT_UNLOCK(_sc)	sx_xunlock(&(_sc)->sc_sx)
#define VPORT_ASSERT(_sc)	sx_assert(&(_sc)->sc_sx, SX_XLOCKED)

static void	vnet_veb_init(const void *);
static void	vnet_veb_uninit(const void *);
static int	veb_clone_create(struct if_clone *, char *, size_t, struct ifc_data *, struct ifnet **);
static int	veb_clone_destroy(struct if_clone *, struct ifnet *, uint32_t);
static void	veb_init(void *);
static void	veb_stop(struct ifnet *);
static int	veb_transmit(struct ifnet *, struct mbuf *);
static void	veb_qflush(struct ifnet *);
static void	veb_timer(void *);
static void	veb_mutecaps(struct veb_softc *);
static void	veb_set_ifcap(struct veb_softc *, struct veb_port *, int);
static int	veb_ioctl(struct ifnet *, u_long, caddr_t);
static void	veb_delete_member(struct veb_softc *, struct veb_port *, int);
static struct mbuf *veb_input(struct ifnet *, struct mbuf *);
static int	veb_output(struct ifnet *, struct mbuf *, struct sockaddr *, struct rtentry *);
static void	veb_forward(struct veb_softc *, struct veb_port *, struct mbuf *);
static void	veb_linkstate(struct ifnet *);
static void	veb_linkcheck(struct veb_softc *);
static struct veb_port *veb_lookup_member(struct veb_softc *, const char *);
static void	veb_ifdetach(void *, struct ifnet *);
static void	veb_broadcast(struct veb_softc *, struct veb_port *, struct ifnet *,struct mbuf *);
static struct veb_port *veb_lookup_member_if(struct veb_softc *, struct ifnet *);
static struct veb_port *veb_port_of(struct ifnet *);
static int	veb_enqueue(struct veb_softc *, struct ifnet *, struct mbuf *,
		    struct veb_port *);
/* replaced ioctl on members */
static int	veb_p_ioctl(struct ifnet *, u_long, caddr_t);

/* Vport methods */
static void	vnet_vport_init(const void *);
static int	vport_clone_create(struct if_clone *, char *, size_t, struct ifc_data *, struct ifnet **);
static int	vport_clone_destroy(struct if_clone *, struct ifnet *, uint32_t);
static int	vport_ioctl(struct ifnet *, u_long, caddr_t);
static void	vport_qflush(struct ifnet *);
static void	vport_init(void *);
static void	vport_stop(struct ifnet *);
static int	vport_transmit(struct ifnet *, struct mbuf *);

/* RT methods */
static int	veb_rtupdate(struct veb_softc *, const uint8_t *,
		    ether_vlanid_t, struct veb_port *, int, uint8_t);
static struct ifnet *veb_rtlookup(struct veb_softc *, const uint8_t *,
		    ether_vlanid_t);
static struct veb_rtnode *veb_rtnode_lookup(struct veb_softc *,
		    const uint8_t *, ether_vlanid_t);
static void	veb_rtage(struct veb_softc *);
static void	veb_rtflush(struct veb_softc *, int);
static void	veb_rtdelete(struct veb_softc *, struct ifnet *, int);
static void	veb_rtable_init(struct veb_softc *);
static void	veb_rtable_fini(struct veb_softc *);
static void	veb_rtnode_destroy(struct veb_softc *, struct veb_rtnode *);
static __inline uint32_t veb_rthash(struct veb_softc *, const uint8_t *);
static int	veb_rtnode_addr_cmp(const uint8_t *, const uint8_t *);
static int	veb_rtnode_insert(struct veb_softc *, struct veb_rtnode *);

/* veb ioctl methods */
static int	veb_ioctl_add(struct veb_softc *, void *);
static int	veb_ioctl_del(struct veb_softc *, void *);
static int	veb_ioctl_gifs(struct veb_softc *, void *);
static int	veb_ioctl_rts(struct veb_softc *, void *);
static int	veb_ioctl_gifflags(struct veb_softc *, void *);
static int	veb_ioctl_sifflags(struct veb_softc *, void *);
static int	veb_ioctl_sto(struct veb_softc *, void *);
static int	veb_ioctl_gto(struct veb_softc *, void *);

#ifdef ALTQ
static void	veb_altq_start(if_t);
static int	veb_altq_transmit(if_t, struct mbuf *);
#endif

#define	VLANTAGOF(_m)	\
    ((_m->m_flags & M_VLANTAG) ? EVL_VLANOFTAG(_m->m_pkthdr.ether_vtag) : DOT1Q_VID_NULL)

static eventhandler_tag veb_detach_cookie;

SYSCTL_DECL(_net_link); // OID_AUTO to not conflict with bridge
static SYSCTL_NODE(_net_link, OID_AUTO, veb, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "veb" );

/* share MAC with first veb member */
VNET_DEFINE_STATIC(int, veb_inherit_mac);
#define	V_veb_inherit_mac	VNET(veb_inherit_mac)
SYSCTL_INT(_net_link_veb, OID_AUTO, inherit_mac,
    CTLFLAG_RWTUN | CTLFLAG_VNET, &VNET_NAME(veb_inherit_mac), 0,
    "Inherit MAC address from the first veb member");

/* log MAC address port flapping */
VNET_DEFINE_STATIC(bool, log_mac_flap) = true;
#define	V_log_mac_flap	VNET(log_mac_flap)
SYSCTL_BOOL(_net_link_veb, OID_AUTO, log_mac_flap,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(log_mac_flap), true,
    "Log MAC address port flapping");

VNET_DEFINE_STATIC(int, rtable_prune_period) = VEB_RTABLE_PRUNE_PERIOD;
#define V_rtable_prune_period VNET(rtable_prune_period)
SYSCTL_INT(_net_link_veb, OID_AUTO, veb_rtable_prune_period,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(rtable_prune_period), true,
    "Number of seconds between walks on the route list");

VNET_DEFINE_STATIC(uma_zone_t, veb_rtnode_zone);
VNET_DEFINE_STATIC(int, log_interval) = 5;
VNET_DEFINE_STATIC(int, log_count) = 0;
VNET_DEFINE_STATIC(struct timeval, log_last) = { 0 };

#define	V_veb_rtnode_zone	VNET(veb_rtnode_zone)
#define	V_log_interval		VNET(log_interval)
#define	V_log_count		VNET(log_count)
#define	V_log_last		VNET(log_last)


struct veb_control {
	int	(*vc_func)(struct veb_softc *, void *);
	int	vc_argsize;
	int	vc_flags;
};

#define VC_F_COPYIN	0x01
#define VC_F_COPYOUT 	0x02
#define VC_F_SUSER 	0x04

static const struct veb_control veb_control_table[] = {
	[VEBADD] = { veb_ioctl_add,	sizeof(struct ifvreq),
			VC_F_COPYIN | VC_F_SUSER },
	[VEBDEL] = { veb_ioctl_del,	sizeof(struct ifvreq),
			VC_F_COPYIN | VC_F_SUSER },
	[VEBGIFS] = { veb_ioctl_gifs,	sizeof(struct ifvpconf),
			VC_F_COPYIN | VC_F_COPYOUT},
	[VEBGRTS] = { veb_ioctl_rts,	sizeof(struct ifvaconf),
			VC_F_COPYIN | VC_F_COPYOUT},
	[VEBGIFFLGS] = { veb_ioctl_gifflags, sizeof(struct ifvreq),
			VC_F_COPYIN | VC_F_COPYOUT},
	[VEBSIFFLGS] = { veb_ioctl_sifflags, sizeof(struct ifvreq),
			VC_F_COPYIN | VC_F_SUSER},
	[VEBGTO] = { veb_ioctl_gto, sizeof(struct ifvrparam),
			VC_F_COPYIN | VC_F_COPYOUT},
	[VEBSTO] = { veb_ioctl_sto, sizeof(struct ifvrparam),
			VC_F_COPYIN | VC_F_SUSER},
};
static const int veb_control_table_size = nitems(veb_control_table);

static void
vnet_vport_init(const void *unused __unused)
{
	struct if_clone_addreq req = {
		.create_f = vport_clone_create,
		.destroy_f = vport_clone_destroy,
		.flags = IFC_F_AUTOUNIT,
	};
	V_vport_cloner = ifc_attach_cloner(vport_name, &req);
}
VNET_SYSINIT(vnet_vport_init, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY,
    vnet_vport_init, NULL);

static int vport_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static int
vport_clone_create(struct if_clone *ifc, char *name, size_t len, struct ifc_data *ifd, struct ifnet **ifpp)
{
	struct vport_softc *sc;
	struct ifnet *ifp;

	sc = malloc(sizeof(*sc), M_VPORT, M_WAITOK | M_ZERO);
	ifp = sc->sc_ifp = if_alloc(IFT_ETHER);

	VPORT_LOCK_INIT(sc);
	ifp->if_softc = sc;
	if_initname(ifp, vport_name, ifd->unit);

	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_capabilities = ifp->if_capenable = IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_MTU;
	ifp->if_ioctl = vport_ioctl;	/* Bare bones ioctl while not attached to veb */
	ifp->if_qflush = vport_qflush;
	ifp->if_init = vport_init;
	ifp->if_transmit = vport_transmit;
	ether_gen_addr(ifp, &sc->sc_defaddr);
	ether_ifattach(ifp, sc->sc_defaddr.octet);
	ifp->if_baudrate = IF_Gbps(10); /* "arbitrary max" - if_epair.c */

	*ifpp = ifp;

	return (0);
}

static void
vport_clone_destroy_cb(struct epoch_context *ctx)
{
	struct vport_softc *sc;

	sc = __containerof(ctx, struct vport_softc, sc_epoch_ctx);

	VPORT_LOCK_DESTROY(sc);
	free(sc, M_VPORT);
}

static int
vport_clone_destroy(struct if_clone *ifc, struct ifnet *ifp, uint32_t flags)
{
	struct vport_softc *sc = if_getsoftc(ifp);

	VPORT_LOCK(sc);
	vport_stop(ifp);
	ifp->if_flags &= ~IFF_UP;
	VPORT_UNLOCK(sc);

	ether_ifdetach(ifp);

	KASSERT(sc->sc_vp == NULL,
		("%s: %s destroyed while still a veb member", __func__, ifp->if_xname));

	if_free(ifp);

	NET_EPOCH_CALL(vport_clone_destroy_cb, &sc->sc_epoch_ctx);

	return (0);
}

/*
 * A vport is the host's port on a veb, so it keeps ordinary interface
 * semantics; only commands that break a veb invariant are gated on
 * membership.
 *
 * Every case sets error and breaks. No case may return directly: the
 * unlock at the bottom is the only release.
 */
static int
vport_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct vport_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *) data;
	int error = 0;

	VPORT_LOCK(sc);

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (!(if_getflags(ifp) & IFF_UP) &&
		   (if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
			vport_stop(ifp);
		} else if ((if_getflags(ifp) & IFF_UP) &&
		   !(if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
		   	VPORT_UNLOCK(sc);
		   	vport_init(sc);
		   	VPORT_LOCK(sc);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	case SIOCSIFMTU:
		if (sc->sc_vp != NULL) {
			error = EXTERROR(EBUSY,
			    "MTU is managed by the veb; set it on the veb");
			break;
		}
		ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCSIFCAP:
		ifp->if_capenable = ifr->ifr_reqcap;
		VLAN_CAPABILITIES(ifp);
		break;
	default:
		if (sc->sc_vp == NULL) {
			error = EXTERROR(EINVAL,
				"Vport cannot be configured when it is not a veb member");
			break;
		}
		VPORT_UNLOCK(sc);
		error = ether_ioctl(ifp, cmd, data);
		VPORT_LOCK(sc);
		break;
	}

	VPORT_UNLOCK(sc);

	return (error);
}

static void
vport_qflush(struct ifnet *ifp)
{
}

static void
vport_init(void *xsc)
{
	struct vport_softc *sc = (struct vport_softc *) xsc;
	struct ifnet *ifp = sc->sc_ifp;

	if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
		return;

	VPORT_LOCK(sc);
	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	VPORT_UNLOCK(sc);
}

static void
vport_stop(struct ifnet *ifp)
{
	struct vport_softc *sc __diagused = if_getsoftc(ifp);

	VPORT_ASSERT(sc);

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
		return;

	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
}

static int
vport_transmit(struct ifnet *ifp, struct mbuf *m)
{
	struct vport_softc *sc = if_getsoftc(ifp);
	struct veb_softc *vebsc;
	struct veb_port *vp;
	struct epoch_tracker et;

	M_ASSERTPKTHDR(m);

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0 ||
	    (ifp->if_flags & IFF_UP) == 0) {
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		return (ENETDOWN);
	}

	/* ether_output() does not tap, and veb_forward() taps sc_ifp. */
	ETHER_BPF_MTAP(ifp, m);

	NET_EPOCH_ENTER(et);

	vp = sc->sc_vp;
	if (vp == NULL) {	/* NULL means we were removed from the veb; no carrier. */
		NET_EPOCH_EXIT(et);
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		return (ENETDOWN);
	}
	vebsc = vp->vp_sc;

	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, m->m_pkthdr.len);
	if (m->m_flags & (M_BCAST | M_MCAST))
		if_inc_counter(ifp, IFCOUNTER_OMCASTS, 1);

	veb_forward(vebsc, vp, m);	/* consumes m */

	NET_EPOCH_EXIT(et);
	return (0);
}

static void
vnet_veb_init(const void *unused __unused)
{

	V_veb_rtnode_zone = uma_zcreate("veb_rtnode",
	    sizeof(struct veb_rtnode), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	struct if_clone_addreq req = {
		.create_f = veb_clone_create,
		.destroy_f = veb_clone_destroy,
		.flags = IFC_F_AUTOUNIT,
	};
	V_veb_cloner = ifc_attach_cloner(veb_name, &req);
}
VNET_SYSINIT(vnet_veb_init, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY,
    vnet_veb_init, NULL);

static void
vnet_veb_uninit(const void *unused __unused)
{
	/*
	 * Order matters: detaching a cloner destroys every instance of it.
	 * veb0 must go first so that veb_delete_member() runs while the
	 * vport ifnets and softcs are still alive.
	 */
	ifc_detach_cloner(V_veb_cloner);
	V_veb_cloner = NULL;
	ifc_detach_cloner(V_vport_cloner);
	V_vport_cloner = NULL;


	/* Callbacks may use the UMA zone. */
	NET_EPOCH_DRAIN_CALLBACKS();

	uma_zdestroy(V_veb_rtnode_zone);
}
VNET_SYSUNINIT(vnet_veb_uninit, SI_SUB_PSEUDO, SI_ORDER_ANY,
    vnet_veb_uninit, NULL);

static int veb_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
		veb_detach_cookie = EVENTHANDLER_REGISTER(
		    ifnet_departure_event, veb_ifdetach, NULL,
		    EVENTHANDLER_PRI_ANY);
		break;
	case MOD_UNLOAD:
		EVENTHANDLER_DEREGISTER(ifnet_departure_event,
		    veb_detach_cookie);
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t veb_mod = {"if_veb", veb_modevent, 0};
DECLARE_MODULE(if_veb, veb_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(if_veb, 1);

static moduledata_t vport_mod = {"if_vport", vport_modevent, 0};
DECLARE_MODULE(if_vport, vport_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(if_vport, 1);

static int
veb_clone_create(struct if_clone *ifc, char *name, size_t len, struct ifc_data *ifd, struct ifnet **ifpp)
{
	struct veb_softc *sc;
	struct ifnet *ifp;

	sc = malloc(sizeof(*sc), M_VEB, M_WAITOK | M_ZERO);
	ifp = sc->sc_ifp = if_alloc(IFT_ETHER);

	VEB_LOCK_INIT(sc);
	sc->sc_vrtmax = VEB_RTABLE_MAX;			/* 2000 */
	sc->sc_vrttimeout = VEB_RTABLE_TIMEOUT;		/* 20 * 60 */

	veb_rtable_init(sc);

	callout_init_mtx(&sc->sc_vebcallout, &sc->sc_rt_mtx, 0);

	CK_LIST_INIT(&sc->sc_vebport);

	ifp->if_softc = sc;
	if_initname(ifp, veb_name, ifd->unit);

	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_capabilities = ifp->if_capenable = IFCAP_VLAN_HWTAGGING;
#ifdef ALTQ
	ifp->if_start = veb_altq_start;
	ifp->if_transmit = veb_altq_transmit;
	IFQ_SET_MAXLEN(&ifp->if_snd, ifqmaxlen);
	ifp->if_snd.ifq_drv_maxlen = 0;
	IFQ_SET_READY(&ifp->if_snd);
#else
	ifp->if_transmit = veb_transmit;
#endif
	ifp->if_ioctl = veb_ioctl;
	ifp->if_qflush = veb_qflush;
	ifp->if_init = veb_init;

	/* 
	 * Having type IFT_BRIDGE is paramount to this driver
	 * as we build off the pre-existing IFT_BRIDGE L2 hookpath in if_ethersubr.c
	 */
	ifp->if_type = IFT_BRIDGE;
	ether_gen_addr(ifp, &sc->sc_defaddr);
	ether_ifattach(ifp, sc->sc_defaddr.octet);
	ifp->if_baudrate = 0; /* Cleanup from ether_ifattach */

	*ifpp = ifp;

	return (0);
}

static void
veb_clone_destroy_cb(struct epoch_context *ctx)
{
	struct veb_softc *sc;

	sc = __containerof(ctx, struct veb_softc, sc_epoch_ctx);

	VEB_LOCK_DESTROY(sc);
	free(sc, M_VEB);
}

static int
veb_clone_destroy(struct if_clone *ifc, struct ifnet *ifp, uint32_t flags)
{
	struct veb_softc *sc = if_getsoftc(ifp);
	struct veb_port *vp;

	VEB_LOCK(sc);

	veb_stop(ifp);
	ifp->if_flags &= ~IFF_UP;

	while ((vp = CK_LIST_FIRST(&sc->sc_vebport)) != NULL)
		veb_delete_member(sc, vp, 0);

	veb_rtable_fini(sc);

	VEB_UNLOCK(sc);

	callout_drain(&sc->sc_vebcallout);

#ifdef ALTQ
	IFQ_PURGE(&ifp->if_snd);
#endif
	ether_ifdetach(ifp);
	if_free(ifp);

	NET_EPOCH_CALL(veb_clone_destroy_cb, &sc->sc_epoch_ctx);

	return (0);
}

static int
veb_transmit(struct ifnet *ifp, struct mbuf *m)
{
        m_freem(m);
        if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
        return (ENETDOWN);
}

static int
veb_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct veb_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *) data;
	struct veb_port *vp;
	struct thread *td = curthread;
	union {
		struct ifvreq		ifvreq;
		struct ifvpconf		ifvpconf;
		struct ifvaconf		ifvaconf;
	} args;
	struct ifdrv *ifd = (struct ifdrv *) data;
	const struct veb_control *vc;
	int error = 0, oldmtu;

	VEB_LOCK(sc);

	switch (cmd) {
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	case SIOCGDRVSPEC:
	case SIOCSDRVSPEC:
		if (ifd->ifd_cmd >= veb_control_table_size) {
			error = EXTERROR(EINVAL, "Invalid control command");
			break;
		}
		vc = &veb_control_table[ifd->ifd_cmd];

		if (vc->vc_func == NULL) {
			error = EXTERROR(EINVAL, "Unimplemented control command");
			break;
		}

		if (cmd == SIOCGDRVSPEC &&
		    (vc->vc_flags & VC_F_COPYOUT) == 0) {
			error = EXTERROR(EINVAL,
			    "Inappropriate ioctl for command "
			    "(expected SIOCSDRVSPEC)");
			break;
		}
		else if (cmd == SIOCSDRVSPEC &&
		    (vc->vc_flags & VC_F_COPYOUT) != 0) {
			error = EXTERROR(EINVAL,
			    "Inappropriate ioctl for command "
			    "(expected SIOCGDRVSPEC)");
			break;
		}

		if (vc->vc_flags & VC_F_SUSER) {
			error = priv_check(td, PRIV_NET_BRIDGE);
			if (error) {
				EXTERROR(error, "PRIV_NET_BRIDGE");
				break;
			}
		}

		if (ifd->ifd_len != vc->vc_argsize ||
		    ifd->ifd_len > sizeof(args)) {
			error = EXTERROR(EINVAL, "Invalid argument size");
			break;
		}

		bzero(&args, sizeof(args));
		if (vc->vc_flags & VC_F_COPYIN) {
			error = copyin(ifd->ifd_data, &args, ifd->ifd_len);
			if (error)
				break;
		}

		oldmtu = if_getmtu(ifp);

		error = (*vc->vc_func)(sc, &args);
		if (error)
			break;

		/*
		 * veb_ioctl_add() adopts the first member's MTU, so a control
		 * command can change ours underneath us.
		 */
		if (if_getmtu(ifp) != oldmtu)
			if_notifymtu(ifp);

		if (vc->vc_flags & VC_F_COPYOUT)
			error = copyout(&args, ifd->ifd_data, ifd->ifd_len);

		break;
	case SIOCSIFFLAGS:
		if (!(if_getflags(ifp) & IFF_UP) &&
		   (if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
			veb_stop(ifp);
		} else if ((if_getflags(ifp) & IFF_UP) &&
		   !(if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
		   	VEB_UNLOCK(sc);
		   	veb_init(sc);
		   	VEB_LOCK(sc);
		}
		break;
	case SIOCSIFMTU:
		oldmtu = sc->sc_ifp->if_mtu;

		if (ifr->ifr_mtu < IF_MINMTU) {
			error = EXTERROR(EINVAL,
			    "Requested MTU is lower than IF_MINMTU");
			break;
		}
		if (CK_LIST_EMPTY(&sc->sc_vebport)) {
			sc->sc_ifp->if_mtu = ifr->ifr_mtu;
			break;
		}
		CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
			if (IS_VPORT(vp)) {
				/* We own this port; no driver to negotiate with. */
				vp->vp_ifp->if_mtu = ifr->ifr_mtu;
				continue;
			}

			error = (*vp->vp_ifp->if_ioctl)(vp->vp_ifp,
			    SIOCSIFMTU, (caddr_t)ifr);
			if (error != 0) {
				log(LOG_NOTICE, "%s: invalid MTU: %u for"
				    " member %s\n", sc->sc_ifp->if_xname,
				    ifr->ifr_mtu,
				    vp->vp_ifp->if_xname);
				error = EINVAL;
				break;
			}
		}
		if (error) {
			/* Restore the previous MTU on all member interfaces. */
			ifr->ifr_mtu = oldmtu;
			CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
				if (IS_VPORT(vp)) {
					/* We own this port; no driver to negotiate with. */
					vp->vp_ifp->if_mtu = ifr->ifr_mtu;
					continue;
				}
				(*vp->vp_ifp->if_ioctl)(vp->vp_ifp,
				    SIOCSIFMTU, (caddr_t)ifr);
			}
			EXTERROR(error,
			    "Failed to set MTU on member interface");
		} else {
			sc->sc_ifp->if_mtu = ifr->ifr_mtu;
		}
		break;

	default:
		VEB_UNLOCK(sc);
		error = ether_ioctl(ifp, cmd, data);
		VEB_LOCK(sc);
		break;
	}

	VEB_UNLOCK(sc);

	return (error);
}

/*
 * veb_p_ioctl
 *
 * Veb members use this ioctl function to
 * be isolated and unaffected by the host
 */
static int
veb_p_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct veb_port *vp = ifp->if_bridge;

	if (vp == NULL)
		return (EOPNOTSUPP);	/* raced with delete */

	switch (cmd) {
	case SIOCSIFFLAGS:	/* ifpromisc() goes through here */
	case SIOCSIFMTU:	/* our own MTU normalization */
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;

	case SIOCSIFADDR:
	case SIOCAIFADDR:
	case SIOCSIFCAP:
		return (EXTERROR(EBUSY, "Interface is a veb member"));

	default:
		if ((cmd & IOC_DIRMASK) == IOC_IN)
			return (EXTERROR(EBUSY, "Interface is a veb member"));
		break;
	}

	if (vp->vp_ioctl == NULL)
		return (EOPNOTSUPP);

	return ((*vp->vp_ioctl)(ifp, cmd, data));
}

/*
 * Resolve an ifnet to its veb_port. Members are found through
 * if_bridge; a vport does not set that field, so it resolves through its
 * own softc.  if_initname() stores the caller's pointer, so if_dname is
 * literally vport_name for every vport.
 *
 * Caller must be in NET_EPOCH or hold VEB_LOCK.
 */
static struct veb_port *
veb_port_of(struct ifnet *member_ifp)
{
	if (strcmp(member_ifp->if_dname, vport_name) == 0) {
		struct vport_softc *vpsc = if_getsoftc(member_ifp);
		return (vpsc->sc_vp);
	}
	return (member_ifp->if_bridge);
}

static struct veb_port *
veb_lookup_member_if(struct veb_softc *sc, struct ifnet *member_ifp)
{
	VEB_LOCK_OR_NET_EPOCH_ASSERT(sc);

	return veb_port_of(member_ifp);
}

static void
veb_delete_member_cb(struct epoch_context *ctx) {
	struct veb_port *vp;

	vp = __containerof(ctx, struct veb_port, vp_epoch_ctx);

	free(vp, M_VEB);
}

static void
veb_delete_member(struct veb_softc *sc, struct veb_port *vp, int gone)
{
	struct ifnet *ifs = vp->vp_ifp;
	struct ifnet *fif = NULL;
	struct veb_port *vpl;
	struct vport_softc *vpsc;

	VEB_ASSERT(sc);

	if (IS_VPORT(vp)) {
		sc->sc_vport = NULL;
		sc->sc_flags &= ~IFVEB_HASVPORT;
		vpsc = if_getsoftc(ifs);
		vpsc->sc_vp = NULL;
	}

	ifs->if_bridge = NULL;

	CK_LIST_REMOVE(vp, vp_next);

	/*
	 * If removing the interface that gave the veb its mac address, set
	 * the mac address of the veb to the address of the next member, or
	 * to its default address if no members are left.
	 */
	if (V_veb_inherit_mac && sc->sc_ifaddr == ifs) {
		if (CK_LIST_EMPTY(&sc->sc_vebport)) {
			bcopy(&sc->sc_defaddr,
			    IF_LLADDR(sc->sc_ifp), ETHER_ADDR_LEN);
			sc->sc_ifaddr = NULL;
		} else {
			vpl = CK_LIST_FIRST(&sc->sc_vebport);
			fif = vpl->vp_ifp;
			bcopy(IF_LLADDR(fif),
			    IF_LLADDR(sc->sc_ifp), ETHER_ADDR_LEN);
			sc->sc_ifaddr = fif;
		}
		EVENTHANDLER_INVOKE(iflladdr_event, sc->sc_ifp);
	}

	veb_linkcheck(sc);
	veb_mutecaps(sc);

	VEB_RT_LOCK(sc);
	veb_rtdelete(sc, ifs, IFVF_FLUSHALL);
	VEB_RT_UNLOCK(sc);

	KASSERT(vp->vp_addrcnt == 0,
	    ("%s: %d veb routes referenced", __func__, vp->vp_addrcnt));

	ifs->if_bridge_output = NULL;
	ifs->if_bridge_input = NULL;
	ifs->if_bridge_linkstate = NULL;

	if (!gone) {
		/* Restore original ioctl function on member interface */
		ifs->if_ioctl = vp->vp_ioctl;
		vp->vp_ioctl = NULL;

		switch (ifs->if_type) {
		case IFT_ETHER:
		case IFT_L2VLAN:
			/*
			 * Take the interface out of promiscuous mode, but only
			 * if it was promiscuous in the first place. It might
			 * not be if we're in the veb_ioctl_add() error path.
			 */
			if (ifs->if_flags & IFF_PROMISC)
				(void) ifpromisc(ifs, 0);
			break;

		default:
			break;
		}
		/* Re-enable any interface capabilities */
		veb_set_ifcap(sc, vp, vp->vp_savedcaps);
	}

	NET_EPOCH_CALL(veb_delete_member_cb, &vp->vp_epoch_ctx);
}

static void
veb_ifdetach(void *arg __unused, struct ifnet *ifp)
{
	struct veb_port *vp = NULL;
	struct veb_softc *sc = NULL;

	vp = veb_port_of(ifp);

	if (vp)
		sc = vp->vp_sc;

	if (V_veb_cloner == NULL) {
		/*
		 * This detach handler can be called after
		 * vnet_veb_uninit().  Just return in that case.
		 */
		return;
	}
	/* Check if the interface is a veb member */
	if (sc != NULL) {
		VEB_LOCK(sc);
		veb_delete_member(sc, vp, 1);
		VEB_UNLOCK(sc);
		return;
	}
}

#ifdef ALTQ
static void
veb_altq_start(if_t ifp)
{
	struct ifaltq *ifq = &ifp->if_snd;
	struct mbuf *m;

	IFQ_LOCK(ifq);
	IFQ_DEQUEUE_NOLOCK(ifq, m);
	while (m != NULL) {
		veb_transmit(ifp, m);
		IFQ_DEQUEUE_NOLOCK(ifq, m);
	}
	IFQ_UNLOCK(ifq);
}

static int
veb_altq_transmit(if_t ifp, struct mbuf *m)
{
	int err;

	if (ALTQ_IS_ENABLED(&ifp->if_snd)) {
		IFQ_ENQUEUE(&ifp->if_snd, m, err);
		if (err == 0)
			veb_altq_start(ifp);
	} else
		err = veb_transmit(ifp, m);

	return (err);
}
#endif	/* ALTQ */

static void
veb_qflush(struct ifnet *ifp __unused)
{
}

static void
veb_init(void *xsc)
{
	struct veb_softc *sc = (struct veb_softc *) xsc;
	struct ifnet *ifp = sc->sc_ifp;

	if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
		return;

	VEB_LOCK(sc);

	callout_reset(&sc->sc_vebcallout, V_rtable_prune_period * hz,
	    veb_timer, sc);

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	VEB_UNLOCK(sc);
}

static void
veb_stop(struct ifnet *ifp)
{
	struct veb_softc *sc = ifp->if_softc;

	VEB_ASSERT(sc);

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
		return;

	VEB_RT_LOCK(sc);

	callout_stop(&sc->sc_vebcallout);

	veb_rtflush(sc, IFVF_FLUSHDYN);
	VEB_RT_UNLOCK(sc);

	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
}

static void
veb_set_ifcap(struct veb_softc *sc, struct veb_port *vp, int set)
{
	struct ifnet *ifp = vp->vp_ifp;
	int (*ioctl_fn)(struct ifnet *, u_long, caddr_t);
	struct ifreq ifr;
	int error, mask, stuck;

	bzero(&ifr, sizeof(ifr));
	ifr.ifr_reqcap = set;

	if (ifp->if_capenable != set) {
		/*
		 * We bypass veb_p_ioctl() here
		 * as it denies SIOCSIFCAP to members.
		 */
		ioctl_fn = (vp->vp_ioctl != NULL) ? vp->vp_ioctl : ifp->if_ioctl;
		error = (*ioctl_fn)(ifp, SIOCSIFCAP, (caddr_t)&ifr);
		if (error)
			if_printf(sc->sc_ifp,
			    "error setting capabilities on %s: %d\n",
			    ifp->if_xname, error);
		mask = VEB_IFCAPS_MASK | VEB_IFCAPS_STRIP;
		stuck = ifp->if_capenable & mask & ~set;
		if (stuck != 0)
			if_printf(sc->sc_ifp,
			    "can't disable some capabilities on %s: 0x%x\n",
			    ifp->if_xname, stuck);
	}
}

static void
veb_mutecaps(struct veb_softc *sc)
{
	struct veb_port *vp;
	int enabled, mask;

	VEB_ASSERT(sc);

	/* Initial bitmask of capabilities to test */
	mask = VEB_IFCAPS_MASK;

	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		/* Every member must support it or it's disabled */
		mask &= vp->vp_savedcaps;
	}

	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		enabled = vp->vp_ifp->if_capenable;
		enabled &= ~VEB_IFCAPS_STRIP;
		/* Strip off mask bits and enable them again if allowed */
		enabled &= ~VEB_IFCAPS_MASK;
		enabled |= mask;
		veb_set_ifcap(sc, vp, enabled);
	}
}

static void
veb_timer(void *arg)
{
	struct veb_softc *sc = arg;

	VEB_RT_LOCK_ASSERT(sc);

	/* Destruction of rtnodes requires a proper vnet context */
	CURVNET_SET(sc->sc_ifp->if_vnet);
	veb_rtage(sc);

	if (sc->sc_ifp->if_drv_flags & IFF_DRV_RUNNING)
		callout_reset(&sc->sc_vebcallout,
		    V_rtable_prune_period * hz, veb_timer, sc);
	CURVNET_RESTORE();
}

static void
veb_rtable_init(struct veb_softc *sc)
{
	int i;

	sc->sc_rthash = malloc(sizeof(*sc->sc_rthash) * VEB_RTHASH_SIZE,
	    M_VEB, M_WAITOK);

	for (i = 0; i < VEB_RTHASH_SIZE; i++)
		CK_LIST_INIT(&sc->sc_rthash[i]);

	sc->sc_rthash_key = arc4random();
	CK_LIST_INIT(&sc->sc_rtlist);
}

static void
veb_rtable_fini(struct veb_softc *sc)
{
	KASSERT(sc->sc_vrtcnt == 0,
	    ("%s: %d veb routes referenced", __func__, sc->sc_vrtcnt));
	free(sc->sc_rthash, M_VEB);
}

static void
veb_rtage(struct veb_softc *sc)
{
	struct veb_rtnode *vrt, *nvrt;

	VEB_RT_LOCK_ASSERT(sc);

	CK_LIST_FOREACH_SAFE(vrt, &sc->sc_rtlist, vrt_list, nvrt) {
		if ((vrt->vrt_flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC) {
			if (time_uptime >= vrt->vrt_expire)
				veb_rtnode_destroy(sc, vrt);
		}
	}
}

static void
veb_rtnode_destroy_cb(struct epoch_context *ctx)
{
	struct veb_rtnode *vrt;

	vrt = __containerof(ctx, struct veb_rtnode, vrt_epoch_ctx);

	CURVNET_SET(vrt->vrt_vnet);
	uma_zfree(V_veb_rtnode_zone, vrt);
	CURVNET_RESTORE();
}

static void
veb_rtnode_destroy(struct veb_softc *sc, struct veb_rtnode *vrt)
{
	VEB_RT_LOCK_ASSERT(sc);

	CK_LIST_REMOVE(vrt, vrt_hash);
	CK_LIST_REMOVE(vrt, vrt_list);

	sc->sc_vrtcnt--;
	vrt->vrt_dst->vp_addrcnt--;

	NET_EPOCH_CALL(veb_rtnode_destroy_cb, &vrt->vrt_epoch_ctx);
}

static void
veb_rtdelete(struct veb_softc *sc, struct ifnet *ifp, int full)
{
	struct veb_rtnode *vrt, *nvrt;

	VEB_RT_LOCK_ASSERT(sc);

	CK_LIST_FOREACH_SAFE(vrt, &sc->sc_rtlist, vrt_list, nvrt) {
		if (vrt->vrt_ifp == ifp && (full ||
			(vrt->vrt_flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC))
			veb_rtnode_destroy(sc, vrt);
	}
}

static void
veb_rtflush(struct veb_softc *sc, int full)
{
	struct veb_rtnode *vrt, *nvrt;

	VEB_RT_LOCK_ASSERT(sc);

	CK_LIST_FOREACH_SAFE(vrt, &sc->sc_rtlist, vrt_list, nvrt) {
		if (full || (vrt->vrt_flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC)
			veb_rtnode_destroy(sc, vrt);
	}
}

static struct ifnet *
veb_rtlookup(struct veb_softc *sc, const uint8_t *addr,
		ether_vlanid_t vlan)
{
	struct veb_rtnode *vrt;

	NET_EPOCH_ASSERT();

	if ((vrt = veb_rtnode_lookup(sc, addr, vlan)) == NULL)
		return (NULL);

	return (vrt->vrt_ifp);
}

static struct veb_rtnode *
veb_rtnode_lookup(struct veb_softc *sc, const uint8_t *addr,
		     ether_vlanid_t vlan)
{
	struct veb_rtnode *vrt;
	uint32_t hash;
	int dir;

	VEB_RT_LOCK_OR_NET_EPOCH_ASSERT(sc);

	hash = veb_rthash(sc, addr);
	CK_LIST_FOREACH(vrt, &sc->sc_rthash[hash], vrt_hash) {
		dir = veb_rtnode_addr_cmp(addr, vrt->vrt_addr);
		if (dir == 0 && (vrt->vrt_vlan == vlan || vlan == DOT1Q_VID_RSVD_IMPL))
			return (vrt);
		if (dir > 0)
			return (NULL);
	}

	return (NULL);
}

/*
 * The following hash function is adapted from "Hash Functions" by Bob Jenkins
 * ("Algorithm Alley", Dr. Dobbs Journal, September 1997).
 */
#define	mix(a, b, c)							\
do {									\
	a -= b; a -= c; a ^= (c >> 13);					\
	b -= c; b -= a; b ^= (a << 8);					\
	c -= a; c -= b; c ^= (b >> 13);					\
	a -= b; a -= c; a ^= (c >> 12);					\
	b -= c; b -= a; b ^= (a << 16);					\
	c -= a; c -= b; c ^= (b >> 5);					\
	a -= b; a -= c; a ^= (c >> 3);					\
	b -= c; b -= a; b ^= (a << 10);					\
	c -= a; c -= b; c ^= (b >> 15);					\
} while (/*CONSTCOND*/0)

static __inline uint32_t
veb_rthash(struct veb_softc *sc, const uint8_t *addr)
{
	uint32_t a = 0x9e3779b9, b = 0x9e3779b9, c = sc->sc_rthash_key;

	b += addr[5] << 8;
	b += addr[4];
	a += addr[3] << 24;
	a += addr[2] << 16;
	a += addr[1] << 8;
	a += addr[0];

	mix(a, b, c);

	return (c & VEB_RTHASH_MASK);
}

#undef mix

static int
veb_rtnode_addr_cmp(const uint8_t *a, const uint8_t *b)
{
	int i, d;

	for (i = 0, d = 0; i < ETHER_ADDR_LEN && d == 0; i++) {
		d = ((int)a[i]) - ((int)b[i]);
	}

	return (d);
}

static int
veb_rtupdate(struct veb_softc *sc, const uint8_t *dst,
		ether_vlanid_t vlan, struct veb_port *vp,
		int setflags, uint8_t flags)
{
	struct veb_rtnode *vrt;
	struct veb_port *ovp;
	int error;

	VEB_LOCK_OR_NET_EPOCH_ASSERT(sc);

	/* Check the source address is valid and not multicast. */
	if (ETHER_IS_MULTICAST(dst))
		return (EXTERROR(EINVAL, "Multicast address not permitted"));
	if (dst[0] == 0 && dst[1] == 0 && dst[2] == 0 &&
	    dst[3] == 0 && dst[4] == 0 && dst[5] == 0)
		return (EXTERROR(EINVAL, "Zero address not permitted"));

	/*
	 * A route for this destination might already exist.  If so,
	 * update it, otherwise create a new one.
	 */
	if ((vrt = veb_rtnode_lookup(sc, dst, vlan)) == NULL) {
		VEB_RT_LOCK(sc);

		/* Check again, now that we have the lock. There could have
		 * been a race and we only want to insert this once. */
		if (veb_rtnode_lookup(sc, dst, vlan) != NULL) {
			VEB_RT_UNLOCK(sc);
			return (0);
		}

		if (sc->sc_vrtcnt >= sc->sc_vrtmax) {
			sc->sc_vrtexceeded++;
			VEB_RT_UNLOCK(sc);
			return (EXTERROR(ENOSPC, "Address table is full"));
		}
		/* Check per interface address limits (if enabled) */
		if (vp->vp_addrmax && vp->vp_addrcnt >= vp->vp_addrmax) {
			vp->vp_addrexceeded++;
			VEB_RT_UNLOCK(sc);
			return (EXTERROR(ENOSPC,
			    "Interface address limit exceeded"));
		}

		/*
		 * Allocate a new bridge forwarding node, and
		 * initialize the expiration time and Ethernet
		 * address.
		 */
		vrt = uma_zalloc(V_veb_rtnode_zone, M_NOWAIT | M_ZERO);
		if (vrt == NULL) {
			VEB_RT_UNLOCK(sc);
			return (EXTERROR(ENOMEM,
			    "Cannot allocate address node"));
		}
		vrt->vrt_vnet = curvnet;

		if (vp->vp_flags & IFVP_STICKY)
			vrt->vrt_flags = IFVAF_STICKY;
		else
			vrt->vrt_flags = IFVAF_DYNAMIC;

		memcpy(vrt->vrt_addr, dst, ETHER_ADDR_LEN);
		vrt->vrt_vlan = vlan;

		vrt->vrt_dst = vp;
		if ((error = veb_rtnode_insert(sc, vrt)) != 0) {
			uma_zfree(V_veb_rtnode_zone, vrt);
			VEB_RT_UNLOCK(sc);
			return (error);
		}
		vp->vp_addrcnt++;

		VEB_RT_UNLOCK(sc);
	}

	if ((vrt->vrt_flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC &&
	    (ovp = vrt->vrt_dst) != vp) {
		MPASS(ovp != NULL);

		VEB_RT_LOCK(sc);
		vrt->vrt_dst->vp_addrcnt--;
		vrt->vrt_dst = vp;
		vrt->vrt_dst->vp_addrcnt++;
		VEB_RT_UNLOCK(sc);

		if (V_log_mac_flap &&
		    ppsratecheck(&V_log_last, &V_log_count, V_log_interval)) {
			log(LOG_NOTICE,
			    "%s: mac address %6D vlan %d moved from %s to %s\n",
			    sc->sc_ifp->if_xname,
			    &vrt->vrt_addr[0], ":",
			    vrt->vrt_vlan,
			    ovp->vp_ifp->if_xname,
			    vp->vp_ifp->if_xname);
		}
	}

	if ((flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC)
		vrt->vrt_expire = time_uptime + sc->sc_vrttimeout;
	if (setflags)
		vrt->vrt_flags = flags;

	return (0);
}

static int
veb_rtnode_insert(struct veb_softc *sc, struct veb_rtnode *vrt)
{
	struct veb_rtnode *lvrt;
	uint32_t hash;
	int dir;

	VEB_RT_LOCK_ASSERT(sc);

	hash = veb_rthash(sc, vrt->vrt_addr);

	lvrt = CK_LIST_FIRST(&sc->sc_rthash[hash]);
	if (lvrt == NULL) {
		CK_LIST_INSERT_HEAD(&sc->sc_rthash[hash], vrt, vrt_hash);
		goto out;
	}

	do {
		dir = veb_rtnode_addr_cmp(vrt->vrt_addr, lvrt->vrt_addr);
		if (dir == 0 && vrt->vrt_vlan == lvrt->vrt_vlan)
			return (EXTERROR(EEXIST, "Address already exists"));
		if (dir > 0) {
			CK_LIST_INSERT_BEFORE(lvrt, vrt, vrt_hash);
			goto out;
		}
		if (CK_LIST_NEXT(lvrt, vrt_hash) == NULL) {
			CK_LIST_INSERT_AFTER(lvrt, vrt, vrt_hash);
			goto out;
		}
		lvrt = CK_LIST_NEXT(lvrt, vrt_hash);
	} while (lvrt != NULL);

#ifdef DIAGNOSTIC
	panic("veb_rtnode_insert: impossible");
#endif

out:
	CK_LIST_INSERT_HEAD(&sc->sc_rtlist, vrt, vrt_list);
	sc->sc_vrtcnt++;

	return (0);
}

static struct veb_port *
veb_lookup_member(struct veb_softc *sc, const char *name)
{
	struct veb_port *vp;
	struct ifnet *ifp;

	VEB_LOCK_OR_NET_EPOCH_ASSERT(sc);

	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		ifp = vp->vp_ifp;
		if (strcmp(ifp->if_xname, name) == 0)
			return (vp);
	}

	return (NULL);
}

static struct mbuf *
veb_input(struct ifnet *ifp, struct mbuf *m) {
	struct veb_softc *sc = NULL;
	struct veb_port *vp;
	struct ifnet *vpp;
	ether_vlanid_t vlan;

	NET_EPOCH_ASSERT();

	/* We need the Ethernet header later, so make sure we have it now. */
	if (m->m_len < ETHER_HDR_LEN) {
		m = m_pullup(m, ETHER_HDR_LEN);
		if (m == NULL) {
			return (NULL);
		}
	}

	vlan = VLANTAGOF(m);

	/*
	 * If this frame has a VLAN tag and the receiving interface has a
	 * vlan(4) trunk, then it is is destined for vlan(4), not for us.
	 * This means if vlan(4) and bridge(4) are configured on the same
	 * interface, vlan(4) is preferred, which is what users typically
	 * expect.
	 */
	if (vlan != DOT1Q_VID_NULL && ifp->if_vlantrunk != NULL)
		return (m);

	vp = ifp->if_bridge;

	/*
	 * If there is no vp, the port has been deleted,
	 * we can carry on as usual
	 */
	if (__predict_false(vp == NULL))
		return (m);
	sc = vp->vp_sc;

	vpp = sc->sc_ifp;
	if ((vpp->if_drv_flags & IFF_DRV_RUNNING) == 0)
		return (m);

	/*
	 * Implement support for veb monitoring. If this flag has been
	 * set on this interface, but before we discard the packet,
	 * increment the byte and packet counters associated with this interface.
	 */
	if ((vpp->if_flags & IFF_MONITOR) != 0) {
		m->m_pkthdr.rcvif  = vpp;
		ETHER_BPF_MTAP(vpp, m);
		if_inc_counter(vpp, IFCOUNTER_IPACKETS, 1);
		if_inc_counter(vpp, IFCOUNTER_IBYTES, m->m_pkthdr.len);
		m_freem(m);
		return (NULL);
	}

	vlan = VLANTAGOF(m);

	/* Perform the bridge forwarding function. */
	veb_forward(sc, vp, m);

	return (NULL);
}

static int
veb_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *sa, struct rtentry *rt) {
	struct ether_header *eh;
	struct veb_port *svp;
	struct ifnet *vpp, *dst_if;
	struct veb_softc *sc;
	ether_vlanid_t vlan;

	NET_EPOCH_ASSERT();

	if (m->m_len < ETHER_HDR_LEN) {
		m = m_pullup(m, ETHER_HDR_LEN);
		if (m == NULL)
			return (0);
	}

	svp = ifp->if_bridge;
	if (__predict_false(svp == NULL)) {
		m_freem(m);
		return (0);
	}
	sc = svp->vp_sc;
	vpp = sc->sc_ifp;

	eh = mtod(m, struct ether_header *);
	vlan = VLANTAGOF(m);

	/*
	 * If veb is down, but the original output interface is up,
	 * go ahead and send out that interface.  Otherwise, the packet
	 * is dropped below.
	 */
	if ((vpp->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		dst_if = ifp;
		goto sendunicast;
	}

	/*
	 * If the packet is a multicast, or we don't know a better way to
	 * get there, send to all interfaces.
	 */
	if (ETHER_IS_MULTICAST(eh->ether_dhost))
		dst_if = NULL;
	else
		dst_if = veb_rtlookup(sc, eh->ether_dhost, vlan);

	/* tap, then broadcast if dst is unknown */
	if (dst_if != ifp)
		ETHER_BPF_MTAP(vpp, m);
	if (dst_if == NULL) {
		veb_broadcast(sc, svp, NULL, m);
		return (0);
	}

sendunicast:
	if ((dst_if->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		m_freem(m);
		return (0);
	}

	veb_enqueue(sc, dst_if, m, NULL);
	return (0);
}

static void
veb_forward(struct veb_softc *sc, struct veb_port *svp, struct mbuf *m)
{
	struct veb_port *dvp;
	struct ifnet *src_if, *dst_if, *ifp;
	struct ether_header *eh;
	uint8_t *dst;
	int error;
	ether_vlanid_t vlan;

	NET_EPOCH_ASSERT();

	src_if = svp->vp_ifp;
	ifp = sc->sc_ifp;
	vlan = VLANTAGOF(m);

	if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_IBYTES, m->m_pkthdr.len);

	eh = mtod(m, struct ether_header *);
	dst = eh->ether_dhost;

	/* If the interface is learning, record the address. */
	if (svp->vp_flags & IFVP_LEARNING) {
		error = veb_rtupdate(sc, eh->ether_shost, vlan,
		    svp, 0, IFVAF_DYNAMIC);
		/*
		 * If the interface has addresses limits then deny any source
		 * that is not in the cache.
		 */
		if (error && svp->vp_addrmax)
			goto drop;
	}

	/*
	 * At this point, the port should be in the forwarding state.
	 */

	/*
	 * If the packet is unicast, destined for someone on
	 * "this" side of the veb, drop it.
	 */
	if ((m->m_flags & (M_BCAST|M_MCAST)) == 0) {
		dst_if = veb_rtlookup(sc, dst, vlan);
		if (src_if == dst_if)
			goto drop;
	} else {
		/*
		 * Check if its a reserved multicast address, any address
		 * listed in 802.1D section 7.12.6 may not be forwarded by the
		 * bridge.
		 * This is currently 01-80-C2-00-00-00 to 01-80-C2-00-00-0F
		 */
		if (dst[0] == 0x01 && dst[1] == 0x80 &&
		    dst[2] == 0xc2 && dst[3] == 0x00 &&
		    dst[4] == 0x00 && dst[5] <= 0x0f)
			goto drop;

		/* ...forward it to all interfaces. */
		if_inc_counter(ifp, IFCOUNTER_IMCASTS, 1);
		dst_if = NULL;
	}


	/*
	 * Since we don't reinject into ether_input, we tap unconditionally
	 */
	ETHER_BPF_MTAP(ifp, m);

	if (dst_if == NULL) {
		veb_broadcast(sc, svp, src_if, m);
		return;
	}

	/*
	 * At this point, we're dealing with a unicast frame
	 * going to a different interface.
	 */
	if ((dst_if->if_drv_flags & IFF_DRV_RUNNING) == 0)
		goto drop;

	dvp = veb_lookup_member_if(sc, dst_if);
	if (dvp == NULL)
		/* Not a member of the veb (anymore?) */
		goto drop;

	/* Private segments can not talk to each other */
	if (svp->vp_flags & dvp->vp_flags & IFVP_PRIVATE)
		goto drop;

	veb_enqueue(sc, dst_if, m, dvp);
	return;

drop:
	m_freem(m);
}

static int
veb_enqueue(struct veb_softc *sc, struct ifnet *dst_ifp, struct mbuf *m,
    struct veb_port *vp)
{
	int len, err = 0;
	short mflags;
	struct mbuf *m0;

	/*
	 * Find the veb member port this packet is being sent on, if the
	 * caller didn't already provide it.
	 */
	if (vp == NULL)
		vp = veb_lookup_member_if(sc, dst_ifp);
	if (vp == NULL) {
		/* Perhaps the interface was removed from the veb */
		m_freem(m);
		return (EINVAL);
	}

	/* We may be sending a fragment so traverse the mbuf */
	for (; m; m = m0) {
		m0 = m->m_nextpkt;
		m->m_nextpkt = NULL;
		len = m->m_pkthdr.len;
		mflags = m->m_flags;

		/*
		 * There are two cases where we have to insert our own tag:
		 * if the member interface doesn't support hardware tagging,
		 * or if the tag proto is not 802.1q.
		 */
		if ((m->m_flags & M_VLANTAG) &&
		    ((dst_ifp->if_capenable & IFCAP_VLAN_HWTAGGING) == 0)) {
			m = ether_vlanencap_proto(m, m->m_pkthdr.ether_vtag,
			    ETHERTYPE_VLAN);
			if (m == NULL) {
				if_printf(dst_ifp,
				    "unable to prepend VLAN header\n");
				if_inc_counter(dst_ifp, IFCOUNTER_OERRORS, 1);
				continue;
			}
			m->m_flags &= ~M_VLANTAG;
		}

		M_ASSERTPKTHDR(m); /* We shouldn't transmit mbuf without pkthdr */

		/*
		 * Host delivery goes to if_input(), not if_transmit(): for a
		 * vport if_transmit() is the host *sending*.
		 *
		 * snd_tag and rcvif share a union in struct pkthdr, and
		 * CSUM_SND_TAG says which is live, so the tag must be released
		 * before rcvif is written or we leak the driver's reference.
		 *
		 * Counters go before if_input(), mbuf is the stack's
		 * afterwards. ether_input() bumps IMCASTS and IBYTES but
		 * never IPACKETS.
		 */
		if (IS_VPORT(vp)) {
			if (m->m_pkthdr.csum_flags & CSUM_SND_TAG) {
				m_snd_tag_rele(m->m_pkthdr.snd_tag);
				m->m_pkthdr.snd_tag = NULL;
				m->m_pkthdr.csum_flags &= ~CSUM_SND_TAG;
			}
			m_tag_delete_nonpersistent(m);
			m->m_pkthdr.rcvif = dst_ifp;
			M_SETFIB(m, dst_ifp->if_fib);

			if_inc_counter(sc->sc_ifp, IFCOUNTER_OPACKETS, 1);
			if_inc_counter(sc->sc_ifp, IFCOUNTER_OBYTES, len);
			if (mflags & M_MCAST)
				if_inc_counter(sc->sc_ifp, IFCOUNTER_OMCASTS, 1);
			if_inc_counter(dst_ifp, IFCOUNTER_IPACKETS, 1);

			dst_ifp->if_input(dst_ifp, m);
			continue;
		}

		if ((err = dst_ifp->if_transmit(dst_ifp, m))) {
			int n;

			for (m = m0, n = 1; m != NULL; m = m0, n++) {
				m0 = m->m_nextpkt;
				m_freem(m);
			}
			if_inc_counter(sc->sc_ifp, IFCOUNTER_OERRORS, n);
			break;
		}

		if_inc_counter(sc->sc_ifp, IFCOUNTER_OPACKETS, 1);
		if_inc_counter(sc->sc_ifp, IFCOUNTER_OBYTES, len);
		if (mflags & M_MCAST)
			if_inc_counter(sc->sc_ifp, IFCOUNTER_OMCASTS, 1);
	}

	return (err);
}

static void
veb_broadcast(struct veb_softc *sc, struct veb_port *svp,
    struct ifnet *exclude_if, struct mbuf *m)
{
	struct veb_port *dvp;
	struct mbuf *mc;
	struct ifnet *dst_if;
	int used = 0;

	NET_EPOCH_ASSERT();

	CK_LIST_FOREACH(dvp, &sc->sc_vebport, vp_next) {
		dst_if = dvp->vp_ifp;
		if (dst_if == exclude_if)
			continue;

		if (svp->vp_flags & dvp->vp_flags & IFVP_PRIVATE)
			continue;

		if ((dvp->vp_flags & IFVP_DISCOVER) == 0 &&
		    (m->m_flags & (M_BCAST|M_MCAST)) == 0)
			continue;

		if ((dst_if->if_drv_flags & IFF_DRV_RUNNING) == 0)
			continue;

		if (CK_LIST_NEXT(dvp, vp_next) == NULL) {
			mc = m;
			used = 1;
		} else {
			mc = m_dup(m, M_NOWAIT);
			if (mc == NULL) {
				if_inc_counter(sc->sc_ifp, IFCOUNTER_OERRORS, 1);
				continue;
			}
		}

		veb_enqueue(sc, dst_if, mc, dvp);
	}
	if (used == 0)
		m_freem(m);
}

static int
veb_ioctl_add(struct veb_softc *sc, void *arg)
{
	struct ifvreq *req = arg;
	struct veb_port *vp = NULL;
	struct vport_softc *vpsc = NULL;
	struct ifnet *ifs;
	int error = 0;

	ifs = ifunit(req->ifvr_ifsname);
	if (ifs == NULL)
		return (EXTERROR(ENOENT, "No such interface",
			req->ifvr_ifsname));

	if (ifs->if_ioctl == NULL)
		return (EXTERROR(EINVAL, "Interface must support ioctl(2)"));


	/* Needed before the veb_port exists, so IS_VPORT() is unavailable. */
	bool is_vport = (strcmp(ifs->if_dname, vport_name) == 0);
	if (is_vport) {
		if (sc->sc_flags & IFVEB_HASVPORT)
			return (EXTERROR(EINVAL, "Veb cannot have multiple vports"));
	}

	/*
	 * If the new interface is a vlan(4), it could be a bridge SVI.
	 * Don't allow such things to be added to bridges.
	 */
	if (ifs->if_type == IFT_L2VLAN) {
		struct ifnet *parent;
		struct epoch_tracker et;
		bool is_bridge;

		/*
		 * Entering NET_EPOCH with VEB_LOCK held, but this is okay
		 * since we don't sleep here.
		 */
		NET_EPOCH_ENTER(et);
		parent = VLAN_TRUNKDEV(ifs);
		is_bridge = (parent != NULL && parent->if_type == IFT_BRIDGE);
		NET_EPOCH_EXIT(et);

		if (is_bridge)
			return (EXTERROR(EINVAL,
			    "SVI cannot be added to a veb"));
	}

	/* Walk our own port list rather than dereferencing if_bridge. */
	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		if (vp->vp_ifp == ifs)
			return (EXTERROR(EEXIST,
			    "Interface is already a member of this veb"));
	}

	if (ifs->if_bridge != NULL)
		return (EXTERROR(EBUSY,
		    "Interface is already a member of another bridge"));
	if (is_vport) {
		vpsc = if_getsoftc(ifs);
		if (vpsc->sc_vp != NULL)
			return (EXTERROR(EBUSY,
			    "Interface is already a member of another veb"));
	}

	switch (ifs->if_type) {
	case IFT_ETHER:
	case IFT_L2VLAN:
		/* permitted interface types */
		break;
	default:
		return (EXTERROR(EINVAL, "Unsupported interface type"));
	}

	/*
	 *  No warning, members can't have IP's when adding no matter what
	 *  Vport member's IP can be configured AFTER adding to a veb
	 */
	struct ifaddr *ifa;

	CK_STAILQ_FOREACH(ifa, &ifs->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family != AF_INET &&
			ifa->ifa_addr->sa_family != AF_INET6)
			continue;

		return (EXTERROR(EINVAL,
			"Member interface may not have "
			"an IP address assigned"));
	}

	/* Allow the first Ethernet member to define the MTU */
	if (CK_LIST_EMPTY(&sc->sc_vebport))
		sc->sc_ifp->if_mtu = ifs->if_mtu;
	else if (sc->sc_ifp->if_mtu != ifs->if_mtu) {
		struct ifreq ifr;

		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s",
		    ifs->if_xname);
		ifr.ifr_mtu = sc->sc_ifp->if_mtu;

		error = (*ifs->if_ioctl)(ifs,
		    SIOCSIFMTU, (caddr_t)&ifr);
		if (error != 0) {
			log(LOG_NOTICE, "%s: invalid MTU: %u for"
			    " new member %s\n", sc->sc_ifp->if_xname,
			    ifr.ifr_mtu,
			    ifs->if_xname);
			return (EXTERROR(EINVAL,
			    "Failed to set MTU on new member"));
		}
	}

	vp = malloc(sizeof(*vp), M_VEB, M_NOWAIT | M_ZERO);
	if (vp == NULL)
		return (ENOMEM);

	vp->vp_sc = sc;
	vp->vp_ifp = ifs;
	vp->vp_flags = IFVP_LEARNING | IFVP_DISCOVER;

	/*
	 * If Interface is a vport,
	 * we add the proper flags and initialize our backpointers
	 */
	if (is_vport) {
		vp->vp_flags |= IFVP_VPORT;
		sc->sc_flags |= IFVEB_HASVPORT;
		vpsc = if_getsoftc(ifs);
		sc->sc_vport = vpsc;
		vpsc->sc_vp = vp;
	}
	vp->vp_savedcaps = ifs->if_capenable;

	/*
	 * Assign the interface's MAC address to the bridge if it's the first
	 * member and the MAC address of the bridge has not been changed from
	 * the default randomly generated one.
	 */
	if (V_veb_inherit_mac && CK_LIST_EMPTY(&sc->sc_vebport) &&
	    !memcmp(IF_LLADDR(sc->sc_ifp), sc->sc_defaddr.octet, ETHER_ADDR_LEN)) {
		bcopy(IF_LLADDR(ifs), IF_LLADDR(sc->sc_ifp), ETHER_ADDR_LEN);
		sc->sc_ifaddr = ifs;
		EVENTHANDLER_INVOKE(iflladdr_event, sc->sc_ifp);
	}

	/*
	 * Reuse ifs->if_bridge as the back-pointer to our port. This isn't only
	 * for convenience: ether_output() and ether_input_internal() both
	 * dispatch on if_bridge != NULL, so a member cannot reach the
	 * forwarding path without it. Setting the field commits us to having
	 * both hooks installed.
	 *
	 * vport's is deliberately excluded. It needs ordinary host semantics
	 * (addressing, SIOCSIFFLAGS, lladdr changes) that veb_p_ioctl() denies
	 * to members, and its ingress is vport_transmit() rather than
	 * veb_output(). One consequence: ifhwioctl()'s "no MTU changes on
	 * bridge members" guard keys off if_bridge, so it does not cover the
	 * vport and vport_ioctl() enforces that itself.
	 */
	if (!IS_VPORT(vp)) {
		ifs->if_bridge_output = veb_output;
		ifs->if_bridge_input = veb_input;
		ifs->if_bridge_linkstate = veb_linkstate;
		ifs->if_bridge = vp;
	}

	CK_LIST_INSERT_HEAD(&sc->sc_vebport, vp, vp_next);

	veb_mutecaps(sc);
	veb_linkcheck(sc);

	switch (ifs->if_type) {
	case IFT_ETHER:
	case IFT_L2VLAN:
		if (!IS_VPORT(vp))
			error = ifpromisc(ifs, 1);
		break;
	}

	/*
	 * Replace interface's ioctl method
	 * with our own to stray away from host
	 */
	vp->vp_ioctl = ifs->if_ioctl;
	if (!IS_VPORT(vp))
		ifs->if_ioctl = veb_p_ioctl;

	if (error)
		veb_delete_member(sc, vp, 0);

	return (error);
}

static int
veb_ioctl_del(struct veb_softc *sc, void *arg)
{
	struct ifvreq *req = arg;
	struct veb_port *vp;

	vp = veb_lookup_member(sc, req->ifvr_ifsname);
	if (vp == NULL)
		return (EXTERROR(ENOENT, "Interface is not a veb member"));

	veb_delete_member(sc, vp, 0);

	return (0);
}

static int
veb_ioctl_gifs(struct veb_softc *sc, void *arg)
{
	struct ifvpconf *vc = arg;
	struct veb_port *vp;
	struct ifvreq vebreq;
	char *buf, *outbuf;
	int count, buflen, len, error = 0;

	count = 0;
	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next)
		count++;

	buflen = sizeof(vebreq) * count;
	if (vc->ifvpc_len == 0) {
		vc->ifvpc_len = buflen;
		return (0);
	}

	/* Return early if there is nothing to malloc */
	if (buflen == 0) {
		vc->ifvpc_len = 0;
		return 0;
	}

	outbuf = malloc(buflen, M_TEMP, M_NOWAIT | M_ZERO);
	if (outbuf == NULL)
		return (ENOMEM);

	count = 0;
	buf = outbuf;
	len = min(vc->ifvpc_len, buflen);
	bzero(&vebreq, sizeof(vebreq));
	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		if (len < sizeof(vebreq))
			break;

		strlcpy(vebreq.ifvr_ifsname, vp->vp_ifp->if_xname,
		    sizeof(vebreq.ifvr_ifsname));

		vebreq.ifvr_ifsflags = vp->vp_flags;

		memcpy(buf, &vebreq, sizeof(vebreq));
		count++;
		buf += sizeof(vebreq);
		len -= sizeof(vebreq);
	}

	vc->ifvpc_len = sizeof(vebreq) * count;
	error = copyout(outbuf, vc->ifvpc_req, vc->ifvpc_len);
	free(outbuf, M_TEMP);
	return (error);
}

static int
veb_ioctl_rts(struct veb_softc *sc, void *arg)
{
	struct ifvaconf *vac = arg;
	struct veb_rtnode *vrt;
	struct ifvareq vareq;
	char *buf, *outbuf;
	int count, buflen, len, error = 0;

	count = 0;
	CK_LIST_FOREACH(vrt, &sc->sc_rtlist, vrt_list)
		count++;
	buflen = sizeof(vareq) * count;

	if (vac->ifvac_len == 0) {
		vac->ifvac_len = buflen;
		return (0);
	}

	if (buflen == 0) {
		vac->ifvac_len = 0;
		return 0;
	}

	outbuf = malloc(buflen, M_TEMP, M_NOWAIT | M_ZERO);
	if (outbuf == NULL)
		return (ENOMEM);

	count = 0;
	buf = outbuf;
	len = min(vac->ifvac_len, buflen);
	bzero(&vareq, sizeof(vareq));
	CK_LIST_FOREACH(vrt, &sc->sc_rtlist, vrt_list) {
		if (len < sizeof(vareq))
			break;
		strlcpy(vareq.ifva_ifsname, vrt->vrt_ifp->if_xname,
		    sizeof(vareq.ifva_ifsname));
		memcpy(vareq.ifva_dst, vrt->vrt_addr, sizeof(vrt->vrt_addr));
		vareq.ifva_vlan = vrt->vrt_vlan;
		if ((vrt->vrt_flags & IFVAF_TYPEMASK) == IFVAF_DYNAMIC &&
				time_uptime < vrt->vrt_expire)
			vareq.ifva_expire = vrt->vrt_expire - time_uptime;
		else
			vareq.ifva_expire = 0;
		vareq.ifva_flags = vrt->vrt_flags;

		memcpy(buf, &vareq, sizeof(vareq));
		count++;
		buf += sizeof(vareq);
		len -= sizeof(vareq);
	}

	vac->ifvac_len = sizeof(vareq) * count;
	error = copyout(outbuf, vac->ifvac_req, vac->ifvac_len);
	free(outbuf, M_TEMP);

	return (error);
}

static int
veb_ioctl_gifflags(struct veb_softc *sc, void *arg)
{
	struct ifvreq *req = arg;
	struct veb_port *vp;

	vp = veb_lookup_member(sc, req->ifvr_ifsname);
	if (vp == NULL)
		return (EXTERROR(ENOENT, "Interface is not a veb member"));

	req->ifvr_ifsflags = vp->vp_flags;

	return (0);
}

static int
veb_ioctl_sifflags(struct veb_softc *sc, void *arg)
{
	struct ifvreq *req = arg;
	struct veb_port *vp;

	vp = veb_lookup_member(sc, req->ifvr_ifsname);
	if (vp == NULL)
		return (EXTERROR(ENOENT, "Interface is not a veb member"));

	if ((req->ifvr_ifssetmask | req->ifvr_ifsclrmask) & ~IFVPUMASK)
		return (EXTERROR(EINVAL, "Unknown port flag"));
	if (req->ifvr_ifssetmask & req->ifvr_ifsclrmask)
		return (EXTERROR(EINVAL, "Flag in both set and clear mask"));

	vp->vp_flags = (vp->vp_flags & ~req->ifvr_ifsclrmask)
			| req->ifvr_ifssetmask;

	return (0);
}

static int
veb_ioctl_sto(struct veb_softc *sc, void *arg)
{
	struct ifvrparam *param = arg;

	sc->sc_vrttimeout = param->ifvrp_ctime;
	return (0);
}

static int
veb_ioctl_gto(struct veb_softc *sc, void *arg)
{
	struct ifvrparam *param = arg;

	param->ifvrp_ctime = sc->sc_vrttimeout;
	return (0);
}

static void
veb_linkstate(struct ifnet *ifp)
{
	struct veb_softc *sc = NULL;
	struct veb_port *vp;
	struct epoch_tracker et;

	NET_EPOCH_ENTER(et);

	vp = veb_port_of(ifp);

	if (vp)
		sc = vp->vp_sc;

	if (sc != NULL) {
		veb_linkcheck(sc);
	}

	NET_EPOCH_EXIT(et);
}

static void
veb_linkcheck(struct veb_softc *sc)
{
	struct veb_port *vp;
	int new_link, hasls;

	VEB_LOCK_OR_NET_EPOCH_ASSERT(sc);

	new_link = LINK_STATE_DOWN;
	hasls = 0;
	/* Our link is considered up if at least one of our ports is active */
	CK_LIST_FOREACH(vp, &sc->sc_vebport, vp_next) {
		/* vport does not contribute to linkstate */
		if (IS_VPORT(vp))
			continue;
		if (vp->vp_ifp->if_capabilities & IFCAP_LINKSTATE)
			hasls++;
		if (vp->vp_ifp->if_link_state == LINK_STATE_UP) {
			new_link = LINK_STATE_UP;
			break;
		}
	}
	if (!CK_LIST_EMPTY(&sc->sc_vebport) && !hasls) {
		/* If no interfaces support link-state then we default to up */
		new_link = LINK_STATE_UP;
	}
	if_link_state_change(sc->sc_ifp, new_link);
}
