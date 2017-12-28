/*
 * Copyright (C) 2017 Matthew Macy <matt.macy@joyent.com>
 * Copyright (C) 2017 Joyent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"


#include <sys/param.h>
#include <sys/types.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/priv.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/gtaskqueue.h>
#include <sys/limits.h>
#include <sys/queue.h>
#include <sys/smp.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if_vlan_var.h>
#include <net/iflib.h>
#include <net/if.h>
#include <net/if_clone.h>
#include <net/route.h>
#include <net/art.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>
#include <netinet6/nd6.h>

#include <net/if_vpc.h>

#include <ck_epoch.h>

#include "ifdi_if.h"

static ck_epoch_t vpc_epoch;

struct vxlanhdr {
    uint32_t reserved0:4;
    uint32_t v_i:1;
    uint32_t reserved1:13;
    uint32_t reserved2:14;
    uint32_t v_vxlanid:24;
    uint32_t reserved3:8;
} __packed;

/*
 * IPv4 w/o VLAN
 */
struct vxlan_header {
    /* outer ether header */
    struct ether_header vh_ehdr;
    /* outer IP header */
    struct ip vh_iphdr;
	/* outer UDP header */
    struct udphdr vh_udphdr;
    /* outer vxlan id header */
    struct vxlanhdr vh_vxlanhdr;
} __packed;


struct vpc_ftable {
	uint32_t vf_vni;
	struct vpc_softc *vf_vs;
	art_tree *vf_ftable;
};

struct egress_cache {
	uint16_t ec_hdr[3];
	uint16_t ec_ifindex;
	int ec_ticks;
	struct vxlan_header ec_vh;
};

struct vf_entry {
	struct sockaddr ve_addr;
};

extern int mp_ncpus;
static int vpc_ifindex_target;
static bool exiting = false;
static struct ifp_cache *vpc_ic;
static struct grouptask vpc_ifp_task;
static struct sx vpc_lock;
SX_SYSINIT(vpc, &vpc_lock, "VPC global");

#define VPC_LOCK() sx_xlock(&vpc_lock)
#define VPC_UNLOCK() sx_xunlock(&vpc_lock)


DPCPU_DEFINE(struct egress_cache *, hdr_cache);
DPCPU_DEFINE(ck_epoch_record_t *, vpc_epoch_record);
ck_epoch_record_t vpc_global_record;

/*
 * ifconfig ixl0 alias 10.1.3.4
 *
 * # Virtual Private Cloud
 * ifconfig vpc0 create
 * ifconfig vpc0 az az0
 * ifconfig vpc0 listen 10.1.3.4:3947
 *
 * ifconfig vpcb0 addm vpc0
 *
 */

static MALLOC_DEFINE(M_VPC, "vpc", "virtual private cloud");

struct ifp_cache {
	uint16_t ic_ifindex_max;
	uint16_t ic_size;
	uint32_t ic_pad;
	struct ifnet *ic_ifps[0];
};

struct vpc_softc {
	if_softc_ctx_t shared;
	if_ctx_t vs_ctx;
	struct sockaddr vs_addr;
	uint16_t vs_vxlan_port;
	uint16_t vs_fibnum;
	uint16_t vs_ifindex_target;
	struct ifp_cache *vs_ic;
	art_tree vs_vxftable; /* vxlanid -> ftable */
	ck_epoch_record_t vs_record;
};

static int clone_count;

static void
m_freechain(struct mbuf *m)
{
	struct mbuf *mp, *mnext;

	mp = m;
	do {
		mnext = mp->m_nextpkt;
		m_freem(mp);
		mp = mnext;
	} while (mp != NULL);
}

static int
hdrcmp(uint16_t *lhs, uint16_t *rhs)
{
	return ((lhs[0] ^ rhs[0]) |
			(lhs[1] ^ rhs[1]) |
			(lhs[2] ^ rhs[2]));
}

static void
_task_fn_ifp_update(void *context __unused)
{
	struct ifnet **ifps, **ifps_orig;
	int i, max, count;

	if (vpc_ifindex_target > vpc_ic->ic_ifindex_max) {
		/* grow and replace after wait */
	}
	max = vpc_ic->ic_ifindex_max;
	ifps = malloc(sizeof(ifps)*max, M_VPC, M_WAITOK|M_ZERO);
	ifps_orig = vpc_ic->ic_ifps;
	for (count = i = 0; i < max; i++) {
		if (ifps_orig[i] == NULL)
			continue;
		if (!(ifps_orig[i]->if_flags & IFF_DYING))
			continue;
		ifps[i] = ifps_orig[i];
		ifps_orig[i] = NULL;
		count++;
	}
	if (count == 0)
		goto done;
	ck_epoch_synchronize(&vpc_global_record);
	for (i = 0; i < max && count; i++){
		if (ifps[i] == NULL)
			continue;
		if_rele(ifps[i]);
		count--;
	}
 done:
	free(ifps, M_VPC);
	if (__predict_false(exiting)) {
		VPC_LOCK();
		free(vpc_ic, M_VPC);
		vpc_ic = NULL;
		wakeup(&exiting);
		VPC_UNLOCK();
	}
}

static struct vpc_ftable *
vpc_vxlanid_lookup(struct vpc_softc *vs, uint32_t vxlanid)
{

	return (art_search(&vs->vs_vxftable, (const unsigned char *)&vxlanid));
}

static int
vpc_ftable_lookup(struct vpc_ftable *vf, struct ether_vlan_header *evh,
				  struct sockaddr *dst)
{
	struct vf_entry *vfe;

	vfe = art_search(vf->vf_ftable, evh->evl_dhost);
	if (__predict_false(vfe == NULL))
		return (ENOENT);
	bcopy(&vfe->ve_addr, dst, sizeof(struct sockaddr *));
	return (0);
}

static uint16_t
vpc_sport_hash(caddr_t data)
{
	uint16_t *hdr;
	uint16_t src, dst;

	hdr = (uint16_t*)data;
	src = hdr[0] ^ hdr[1] ^ hdr[2];
	dst = hdr[3] ^ hdr[4] ^ hdr[5];
	return (src ^ dst);
}

static void
vpc_vxlanhdr_init(struct vpc_ftable *vf, struct vxlan_header *vh, 
				  struct sockaddr *dstip, struct ifnet *ifp, struct mbuf *m)
{
	struct sockaddr_in *sin;
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *uh;
	struct vxlanhdr *vhdr;
	caddr_t smac;

	smac = ifp->if_hw_addr;
	eh = &vh->vh_ehdr;
	eh->ether_type = htons(ETHERTYPE_IP); /* v4 only to start */
	/* arp resolve fills in dest */
	bcopy(smac, eh->ether_shost, ETHER_ADDR_LEN);

	ip = (struct ip *)(uintptr_t)&vh->vh_iphdr;
	ip->ip_hl = sizeof(*ip) >> 2;
	ip->ip_v = 4; /* v4 only now */
	ip->ip_tos = 0;
	/* XXX validate that we won't overrun IP_MAXPACKET first */
	ip->ip_len = m->m_pkthdr.len + sizeof(*vh) - sizeof(struct ether_header);
	ip->ip_id = 0;
	ip->ip_off = 0;
	ip->ip_ttl = 255;
	ip->ip_p = IPPROTO_UDP;
	ip->ip_sum = 0;
	sin = (struct sockaddr_in *)&vf->vf_vs->vs_addr;
	ip->ip_src.s_addr = sin->sin_addr.s_addr;
	sin = (struct sockaddr_in *)dstip;
	ip->ip_dst.s_addr = sin->sin_addr.s_addr;
	/* check that CSUM_IP works for all hardware */

	uh = (struct udphdr*)(uintptr_t)&vh->vh_udphdr;
	uh->uh_sport = vpc_sport_hash(m->m_next->m_data);
	uh->uh_dport = vf->vf_vs->vs_vxlan_port;
	uh->uh_ulen = ip->ip_len - sizeof(*ip);
	uh->uh_sum = 0; /* offload */

	vhdr = (struct vxlanhdr *)(uintptr_t)&vh->vh_vxlanhdr;
	vhdr->v_i = 1;
	vhdr->v_vxlanid = htonl(vf->vf_vni) >> 8;
}

static int
vpc_cache_lookup(struct vpc_softc *vs, struct mbuf *m, struct ether_vlan_header *evh)
{
	struct egress_cache *ecp;
	struct ifnet *ifp;

	_critical_enter();
	ecp = DPCPU_GET(hdr_cache);
	if (__predict_false(ecp->ec_ticks == 0))
		goto skip;
	/*
	 * Is still in caching window
	 */
	if (__predict_false(ticks - ecp->ec_ticks < hz/5)) {
		ecp->ec_ticks = 0;
		goto skip;
	}
	ifp = vs->vs_ic->ic_ifps[ecp->ec_ifindex];
	if (ifp == NULL) {
		ecp->ec_ticks = 0;
		goto skip;
	}
	if (ifp->if_flags & IFF_DYING) {
		ecp->ec_ticks = 0;
		GROUPTASK_ENQUEUE(&vpc_ifp_task);
		goto skip;
	}
	/*
	 * dmac & vxlanid match
	 */
	if (hdrcmp(ecp->ec_hdr, (uint16_t *)evh->evl_dhost) == 0 &&
		(m->m_pkthdr.vxlanid == ecp->ec_vh.vh_vxlanhdr.v_vxlanid)) {
		/* re-use last header */
		bcopy(&ecp->ec_vh, m->m_data, sizeof(struct vxlan_header));
		_critical_exit();
		m->m_pkthdr.rcvif = ifp;
		return (1);
	}
	skip:
	_critical_exit();
	return (0);
}

static void
vpc_cache_update(struct mbuf *m, struct ether_vlan_header *evh, uint16_t ifindex)
{
	struct egress_cache *ecp;
	uint16_t *src;

	src = (uint16_t *)evh->evl_dhost;
	_critical_enter();
	/* update pcpu cache */
	ecp = DPCPU_GET(hdr_cache);
	ecp->ec_hdr[0] = src[0];
	ecp->ec_hdr[1] = src[1];
	ecp->ec_hdr[2] = src[2];
	bcopy(m->m_data, &ecp->ec_vh, sizeof(struct vxlan_header));
	ecp->ec_ticks = ticks;
	ecp->ec_ifindex = ifindex;
	_critical_exit();
}

static int
vpc_ifp_cache(struct vpc_softc *vs, struct ifnet *ifp)
{
	struct ifnet **ifps;

	ifps = vs->vs_ic->ic_ifps;
	if (__predict_false(vs->vs_ic->ic_ifindex_max < ifp->if_index)) {
		ifps = realloc(ifps, sizeof(ifp)*ifp->if_index, M_VPC, M_NOWAIT);
		if (ifps != NULL)
			vs->vs_ic->ic_ifindex_max = ifp->if_index;
	}

	if (__predict_false(ifps == NULL)) {
		vs->vs_ifindex_target = roundup(ifp->if_index, 512);
		GROUPTASK_ENQUEUE(&vpc_ifp_task);
		return (1);
	}
	if (ifps[ifp->if_index] == ifp)
		return (0);
	if_ref(ifp);
	ifps[ifp->if_index] = ifp;
	return (0);
}

static struct mbuf *
vpc_vxlan_encap(struct vpc_softc *vs, struct mbuf *m)
{
	struct ether_vlan_header *evh, *evhvx;
	struct vxlan_header *vh;
	struct mbuf *mh;
	struct vpc_ftable *vf;
	struct sockaddr *dst;
	struct route ro;
	struct rtentry *rt;
	struct ifnet *ifp;
	int rc;

	mh = m_gethdr(M_NOWAIT, MT_NOINIT);
	if (__predict_false(mh == NULL)) {
		m_freem(m);
		return (NULL);
	}
	evhvx = (struct ether_vlan_header *)m->m_data;
	bcopy(&m->m_pkthdr, &mh->m_pkthdr, sizeof(struct pkthdr));
	mh->m_data = m->m_pktdat;
	vh = (struct vxlan_header *)mh->m_data;
	evh = (struct ether_vlan_header *)&vh->vh_ehdr;
	mh->m_flags &= ~(M_EXT|M_NOFREE|M_VLANTAG|M_BCAST|M_MCAST|M_TSTMP);
	mh->m_pkthdr.len += sizeof(struct vxlan_header);
	mh->m_len = sizeof(struct vxlan_header);
	mh->m_next = m;
	m->m_pkthdr.csum_flags = CSUM_IP|CSUM_UDP;
	m->m_pkthdr.csum_data = offsetof(struct udphdr, uh_sum);
	m->m_nextpkt = NULL;
	m->m_flags &= ~(M_PKTHDR|M_NOFREE|M_VLANTAG|M_BCAST|M_MCAST|M_TSTMP);

	if (__predict_true(vpc_cache_lookup(vs, mh, evhvx)))
		return (mh);

	/* lookup MAC->IP forwarding table */
	vf = vpc_vxlanid_lookup(vs, m->m_pkthdr.vxlanid);
	if (__predict_false(vf == NULL))
		return (NULL);

	dst = &ro.ro_dst;
	/*   lookup IP using encapsulated dmac */
	rc = vpc_ftable_lookup(vf, evhvx, dst);
	if (__predict_false(rc))
		return (NULL);
	/* lookup route to find interface */
	rt = rtalloc1_fib(dst, 0, 0, vs->vs_fibnum);
	if (__predict_false(rt == NULL))
		return (NULL);
	if (__predict_false(!(rt->rt_flags & RTF_UP) ||
						(rt->rt_ifp == NULL) ||
						!RT_LINK_IS_UP(rt->rt_ifp))) {
		RTFREE_LOCKED(rt);
		return (NULL);
	}
	ifp = rt->rt_ifp;
	rc = vpc_ifp_cache(vs, ifp);
	RTFREE_LOCKED(rt);

	if (__predict_false(rc))
		return (NULL);

	/* get dmac */
	switch(dst->sa_family) {
		case AF_INET:
			rc = arpresolve(ifp, 0, NULL, dst,
							evh->evl_dhost, NULL, NULL);
			break;
		case AF_INET6:
			rc = nd6_resolve(ifp, 0, NULL, dst,
							 evh->evl_dhost, NULL, NULL);
			break;
		default:
			rc = EOPNOTSUPP;
	}
	if (__predict_false(rc))
		return (NULL);
	mh->m_pkthdr.rcvif = ifp;
	vpc_vxlanhdr_init(vf, vh, dst, ifp, m);
	vpc_cache_update(mh, evhvx, ifp->if_index);
	return (mh);
}

static struct mbuf *
vpc_vxlan_encap_chain(struct vpc_softc *vs, struct mbuf *m, bool *can_batch)
{
	struct mbuf *mh, *mt, *mnext;
	struct ifnet *ifp;

	mh = mt = NULL;
	*can_batch = true;
	do {
		mnext = m->m_nextpkt;
		m->m_nextpkt = NULL;
		m = vpc_vxlan_encap(vs, m);
		if (m == NULL)
			break;
		if (mh != NULL) {
			if (ifp != m->m_pkthdr.rcvif)
				*can_batch = false;
			mt->m_nextpkt = m;
			mt = m;
		} else {
			ifp = m->m_pkthdr.rcvif;
			mh = mt = m;
		}
		m = mnext;
	} while (m != NULL);
	if (__predict_false(mnext != NULL))
		m_freechain(mnext);
	return (mh);
}

static int
vpc_transmit(if_t ifp, struct mbuf *m)
{
	if_ctx_t ctx = ifp->if_softc;
	struct vpc_softc *vs = iflib_get_softc(ctx);
	struct mbuf *mp, *mnext;
	bool can_batch;
	int lasterr, rc;

	can_batch = true;
	if ((m->m_flags & M_VXLANTAG) == 0)
		m_freechain(m);
	_critical_enter();
	sched_pin();
	ck_epoch_begin(DPCPU_GET(vpc_epoch_record), NULL);
	_critical_exit();

	m = vpc_vxlan_encap_chain(vs, m, &can_batch);
	if (__predict_false(m == NULL)) {
		lasterr = ENXIO;
		goto done;
	}
	ifp = m->m_pkthdr.rcvif;
	if (__predict_true((ifp->if_capabilities & IFCAP_TXBATCH) && can_batch)) {
		lasterr = ifp->if_transmit(ifp, m);
		goto done;
	}

	mp = m;
	lasterr = 0;
	do {
		mnext = mp->m_nextpkt;
		mp->m_nextpkt = NULL;
		ifp = m->m_pkthdr.rcvif;
		rc = ifp->if_transmit(ifp, m);
		if (rc)
			lasterr = rc;
		mp = mnext;
	} while (mp != NULL);
 done:
	_critical_enter();
	ck_epoch_end(DPCPU_GET(vpc_epoch_record), NULL);
	sched_unpin();
	_critical_exit();
	return (lasterr);
}

static int
vpc_cloneattach(if_ctx_t ctx, struct if_clone *ifc, const char *name, caddr_t params)
{
	struct vpc_softc *vs = iflib_get_softc(ctx);
	if_softc_ctx_t scctx;

	atomic_add_int(&clone_count, 1);
	scctx = vs->shared = iflib_get_softc_ctx(ctx);

	/* register vs_record */
	ck_epoch_register(&vpc_epoch, &vs->vs_record, NULL);
	vs->vs_ctx = ctx;

	/* init vs_vxftable */
	art_tree_init(&vs->vs_vxftable, 4);
	return (0);
}

static int
vpc_attach_post(if_ctx_t ctx)
{
	struct ifnet *ifp;

	ifp = iflib_get_ifp(ctx);

	ifp->if_transmit = vpc_transmit;
	return (0);
}

static int
vpc_detach(if_ctx_t ctx)
{
	atomic_add_int(&clone_count, -1);
	return (0);
}

static void
vpc_init(if_ctx_t ctx)
{
}

static void
vpc_stop(if_ctx_t ctx)
{
}

static int
vpc_set_listen(struct vpc_softc *vs, struct vpc_listen *vl)
{
	struct route ro;
	struct ifnet *ifp;
	struct ifreq ifr;
	struct rtentry *rt;
	int rc;

	rc = 0;

	vs->vs_vxlan_port = vl->vl_port;
	bzero(&ro, sizeof(ro));
	bcopy(&vl->vl_addr, &ro.ro_dst, sizeof(struct sockaddr));
	/* lookup route to find interface */
	in_rtalloc_ign(&ro, 0, vs->vs_fibnum);
	rt = ro.ro_rt;
	if (__predict_false(rt == NULL))
		return (ENETUNREACH);
	if (__predict_false(!(rt->rt_flags & RTF_UP) ||
						(rt->rt_ifp == NULL))) {
		rc = ENETUNREACH;
		goto fail;
	}
	ifp = rt->rt_ifp;
	if (!(ifp->if_capabilities & IFCAP_VXLANDECAP)) {
		rc = EOPNOTSUPP;
		goto fail;
	}
	ifr.ifr_index = vs->vs_vxlan_port;
	rc = ifp->if_ioctl(ifp, SIOCSIFVXLANPORT, (caddr_t)&ifr);

 fail:
	RTFREE(rt);
	return (rc);
}

static int
vpc_priv_ioctl(if_ctx_t ctx, u_long command, caddr_t data)
{
	struct vpc_softc *vs = iflib_get_softc(ctx);
	struct ifreq *ifr = (struct ifreq *)data;
	struct ifreq_buffer *ifbuf = &ifr->ifr_ifru.ifru_buffer;
	struct vpc_ioctl_header *ioh =
	    (struct vpc_ioctl_header *)(ifbuf->buffer);
	int rc = ENOTSUP;
	struct vpc_ioctl_data *iod = NULL;

	if (command != SIOCGPRIVATE_0)
		return (EINVAL);

	if ((rc = priv_check(curthread, PRIV_DRIVER)) != 0)
		return (rc);
#ifdef notyet
	/* need sx lock for iflib context */
	iod = malloc(ifbuf->length, M_VPC, M_WAITOK | M_ZERO);
#endif
	iod = malloc(ifbuf->length, M_VPC, M_NOWAIT | M_ZERO);
	copyin(ioh, iod, ifbuf->length);

	switch (ioh->vih_type) {
		case VPC_LISTEN:
			rc = vpc_set_listen(vs, (struct vpc_listen *)iod);
			break;
#ifdef notyet
		case VPC_VNI_ADD:
		case VPC_VNI_DEL:
		case VPC_VNI_ALL:
		case VPC_FTE_ADD:
		case VPC_FTE_DEL:
		case VPC_FTE_ALL:
			break;
#endif
		default:
			rc = ENOIOCTL;
			break;
	}
	free(iod, M_VPC);
	return (rc);
}

static device_method_t vpc_if_methods[] = {
	DEVMETHOD(ifdi_cloneattach, vpc_cloneattach),
	DEVMETHOD(ifdi_attach_post, vpc_attach_post),
	DEVMETHOD(ifdi_detach, vpc_detach),
	DEVMETHOD(ifdi_init, vpc_init),
	DEVMETHOD(ifdi_stop, vpc_stop),
	DEVMETHOD(ifdi_priv_ioctl, vpc_priv_ioctl),
	DEVMETHOD_END
};

static driver_t vpc_iflib_driver = {
	"vpc", vpc_if_methods, sizeof(struct vpc_softc)
};

char vpc_driver_version[] = "0.0.1";

static struct if_shared_ctx vpc_sctx_init = {
	.isc_magic = IFLIB_MAGIC,
	.isc_driver_version = vpc_driver_version,
	.isc_driver = &vpc_iflib_driver,
	.isc_flags = 0,
	.isc_name = "vpc",
};

if_shared_ctx_t vpc_sctx = &vpc_sctx_init;


#define IC_START_COUNT 512
static if_pseudo_t vpc_pseudo;	

static int
vpc_module_init(void)
{
	struct egress_cache **ecpp, *ecp;
	ck_epoch_record_t **erpp, *erp;
	int i, ec_size, er_size;

	vpc_pseudo = iflib_clone_register(vpc_sctx);
	if (vpc_pseudo == NULL)
		return (ENXIO);
	ck_epoch_init(&vpc_epoch);
	ck_epoch_register(&vpc_epoch, &vpc_global_record, NULL);
	iflib_config_gtask_init(NULL, &vpc_ifp_task, _task_fn_ifp_update, "ifp update");

	/* DPCPU hdr_cache init */
	/* DPCPU vpc epoch record init */
	ec_size = roundup(sizeof(*ecp), CACHE_LINE_SIZE);
	er_size = roundup(sizeof(*erp), CACHE_LINE_SIZE);

	ecp = malloc(ec_size*mp_ncpus, M_VPC, M_WAITOK|M_ZERO);
	erp = malloc(er_size*mp_ncpus, M_VPC, M_WAITOK);
	vpc_ic = malloc(sizeof(uint64_t) + (sizeof(struct ifnet *)*IC_START_COUNT),
					M_VPC, M_WAITOK|M_ZERO);
	vpc_ic->ic_size = IC_START_COUNT;

	CPU_FOREACH(i) {
		ck_epoch_register(&vpc_epoch, erp, NULL);

		ecpp = DPCPU_ID_PTR(i, hdr_cache);
		erpp = DPCPU_ID_PTR(i, vpc_epoch_record);
		*ecpp = ecp;
		*erpp = erp;
		ecp = (struct egress_cache *)(((caddr_t)ecp) + ec_size);
		erp = (ck_epoch_record_t *)(((caddr_t)erp) + er_size);
	}

	return (0);
}

static void
vpc_module_deinit(void)
{
	struct egress_cache *ecp;
	ck_epoch_record_t *erp;

	exiting = true;
	VPC_LOCK();
	GROUPTASK_ENQUEUE(&vpc_ifp_task);
	sx_sleep(&exiting, &vpc_lock, PDROP, "vpc exiting", 0);
	ecp = DPCPU_ID_GET(0, hdr_cache);
	erp = DPCPU_ID_GET(0, vpc_epoch_record);
	free(ecp, M_VPC);
	free(erp, M_VPC);
	iflib_clone_deregister(vpc_pseudo);
}


static int
vpc_module_event_handler(module_t mod, int what, void *arg)
{
	int err;

	switch (what) {
		case MOD_LOAD:
			if ((err = vpc_module_init()) != 0)
				return (err);
			break;
		case MOD_UNLOAD:
			if (clone_count == 0)
				vpc_module_deinit();
			else
				return (EBUSY);
			break;
		default:
			return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t vpc_moduledata = {
	"vpc",
	vpc_module_event_handler,
	NULL
};

DECLARE_MODULE(vpc, vpc_moduledata, SI_SUB_INIT_IF, SI_ORDER_ANY);
MODULE_VERSION(vpc, 1);
MODULE_DEPEND(vpc, iflib, 1, 1, 1);
