/*
 * Copyright (C) 2017-2018 Matthew Macy <matt.macy@joyent.com>
 * Copyright (C) 2017-2018 Joyent Inc.
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

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/smp.h>
#include <sys/sysctl.h>


#ifdef MVEC_DEBUG
#define  DPRINTF printf
#else
#define DPRINTF(...)
#endif

static MALLOC_DEFINE(M_MVEC, "mvec", "mbuf vector");

static int type2len[] = {-1, MCLBYTES, -1, MJUMPAGESIZE, MJUM9BYTES, MJUM16BYTES, -1, MSIZE};
#ifdef INVARIANTS
static int validtypes = ((1<<EXT_CLUSTER)|(1<<EXT_JUMBOP)|(1<<EXT_JUMBO9)|(1<<EXT_JUMBO16)|(1<<EXT_MBUF));
#endif

#define ADDCARRY(x)  (x > 65535 ? x -= 65535 : x)
#define REDUCE32							  \
    {									  \
	q_util.q = sum;							  \
	sum = q_util.s[0] + q_util.s[1] + q_util.s[2] + q_util.s[3];	  \
    }
#define REDUCE16							  \
    {									  \
	q_util.q = sum;							  \
	l_util.l = q_util.s[0] + q_util.s[1] + q_util.s[2] + q_util.s[3]; \
	sum = l_util.s[0] + l_util.s[1];				  \
	ADDCARRY(sum);							  \
    }

uint64_t in_cksumdata(const void *buf, int len);

union l_util {
	u_int16_t s[2];
	u_int32_t l;
};
union q_util {
	u_int16_t s[4];
	u_int32_t l[2];
	u_int64_t q;
};


#ifdef INVARIANTS
void
mvec_sanity(const struct mbuf *m)
{
	const struct mbuf_ext *mext;
	const struct mvec_header *mh;
	const struct mvec_ent *me;
	const m_refcnt_t *me_count;
	int i, total;

	mext = (const void*)m;
	mh = &mext->me_mh;
	me = &mext->me_ents[mh->mh_start];

	MPASS((const char *)me != m->m_data);
	me_count = &((const m_refcnt_t *)(mext->me_ents + mh->mh_count))[mh->mh_start];
	MPASS(me_count == &MBUF2REF((uintptr_t)m)[mh->mh_start]);
	total = 0;
	MPASS(m->m_len == me->me_len);
	MPASS(m->m_data == (me->me_cl + me->me_off));
	MPASS((m->m_flags & (M_EXT|M_PKTHDR|M_NOFREE|M_UNUSED_8)) == (M_EXT|M_PKTHDR));
	MPASS(mh->mh_count >= (mh->mh_start + mh->mh_used));
	for (i = mh->mh_start; i < mh->mh_used + mh->mh_start; i++, me++, me_count++) {
		if (__predict_false(me->me_len == 0)) {
			if (mh->mh_multiref)
				MPASS(me_count->ext_cnt == NULL);
			continue;
		}
		if (mh->mh_multiref) {
			if (me->me_type == MVEC_MANAGED)
				MPASS(me_count->ext_cnt != NULL);
			else
				MPASS(me_count->ext_cnt == NULL);
		}

		MPASS(me->me_cl);
		MPASS(me->me_cl != (const void *)0xdeadc0dedeadc0de);
		total += me->me_len;
	}
	MPASS(total == m->m_pkthdr.len);
}
#endif

void
mvec_buffer_free(struct mbuf *m)
{
	struct mvec_header *mh;

	mh = MBUF2MH(m);
	switch (mh->mh_mvtype) {
		case MVALLOC_MALLOC:
			free(m, M_MVEC);
			break;
		case MVALLOC_MBUF:
			uma_zfree_arg(zone_mbuf, m, (void *)MB_DTOR_SKIP);
			break;
		case MVALLOC_CLUSTER:
			uma_zfree(zone_clust, m);
			break;
		default:
			panic("unrecognized mvalloc value: %d\n", mh->mh_mvtype);
	}
}

static void
mvec_clfree(struct mvec_ent *me, m_refcnt_t *refcntp, bool dupref)
{
	bool free = true;
	struct mbuf *mref;
	volatile uint32_t *refcnt;

	mref = NULL;
	if (dupref) {
		if (me->me_ext_flags & EXT_FLAG_EMBREF) {
			refcnt = &refcntp->ext_count;
		} else {
			refcnt = refcntp->ext_cnt;
		}
		free = (*refcnt == 1 || atomic_fetchadd_int(refcnt, -1) == 1);
	}
	if (!free)
		return;
	if (!(me->me_ext_flags & EXT_FLAG_NOFREE))
		mref =  __containerof(refcnt, struct mbuf, m_ext.ext_count);

	switch (me->me_ext_type) {
		case EXT_CLUSTER:
			uma_zfree(zone_clust, me->me_cl);
			break;
		case EXT_JUMBOP:
			uma_zfree(zone_jumbop, me->me_cl);
			break;
		case EXT_JUMBO9:
			uma_zfree(zone_jumbo9, me->me_cl);
			break;
		case EXT_JUMBO16:
			uma_zfree(zone_jumbo16, me->me_cl);
			break;
		default:
			panic("unsupported ext_type: %d\n", me->me_ext_type);
	}
	if (mref != NULL)
		uma_zfree_arg(zone_mbuf, mref, (void *)MB_DTOR_SKIP);
}

static void
mvec_ent_free(struct mvec_header *mh, int idx)
{
	struct mvec_ent *me = (struct mvec_ent *)(mh + 1);
	m_refcnt_t *me_count = (m_refcnt_t *)(me + mh->mh_count);

	me += idx;
	me_count += idx;
	switch (me->me_type) {
		case MVEC_MBUF:
			uma_zfree_arg(zone_mbuf, me->me_cl, (void *)MB_DTOR_SKIP);
			break;
		case MVEC_MANAGED:
			mvec_clfree(me, me_count, mh->mh_multiref);
			break;
		default:
			/* ... */
			break;
	}
}

void *
mvec_seek(const struct mbuf *m, struct mvec_cursor *mc, int offset)
{
	const struct mbuf_ext *mext = (const struct mbuf_ext *)m;
	const struct mvec_ent *me = mext->me_ents;
	const struct mvec_header *mh = &mext->me_mh;
	int rem;

	mc->mc_idx = mc->mc_off = 0;
	if (offset >= m->m_pkthdr.len)
		return (NULL);
	rem = offset;

	me += mh->mh_start;
	MPASS(me->me_len);
	mc->mc_off = offset;
	while (mc->mc_off >= me->me_len) {
		mc->mc_off -= me->me_len;
		mc->mc_idx++;
		me++;
	}
	return (void *)(me_data(me) + mc->mc_off);
}

void *
mvec_seek_pktno(const struct mbuf *m, struct mvec_cursor *mc, int offset, uint16_t pktno)
{
	const struct mbuf_ext *mext = (const struct mbuf_ext *)m;
	const struct mvec_ent *me = mext->me_ents;
	const struct mvec_header *mh = &mext->me_mh;
	int i, rem, pktcur;

	pktcur = mc->mc_off = 0;
	MPASS(offset <= m->m_pkthdr.len);
	rem = offset;

	me += mh->mh_start;
	for (i = 0; i < mh->mh_used && pktcur < pktno; i++, me++)
		if (me->me_eop)
			pktcur++;
	if (pktcur < pktno)
		return (NULL);
	mc->mc_idx = i;
	while (mc->mc_off >= me->me_len) {
		if (me->me_eop)
			return (NULL);
		mc->mc_off -= me->me_len;
		mc->mc_idx++;
		me++;
	}
	return (void *)(me_data(me) + mc->mc_off);
}

uint32_t
mvec_pktlen(const struct mbuf *m, struct mvec_cursor *mc_, int pktno)
{
	const struct mbuf_ext *mext = (const struct mbuf_ext *)m;
	const struct mvec_ent *me = mext->me_ents;
	const struct mvec_header *mh = &mext->me_mh;
	struct mvec_cursor mc, *mcp;
	int i, len, maxsegs;
	void *p;

	mcp = mc_ ? mc_ : &mc;
	len = mcp->mc_off = mcp->mc_idx = 0;
	if (pktno >= 0) {
		p = mvec_seek_pktno(m, mcp, 0, pktno);
		if (__predict_false(p == NULL))
			return (0);
	}
	me += (mh->mh_start + mcp->mc_idx);
	maxsegs = mh->mh_used - mcp->mc_idx;
	for (i = 0; i < maxsegs; i++, me++) {
		len += me->me_len;
		if (me->me_eop)
			break;
	}
	return (len);
}

static void
mvec_trim_head(struct mbuf *m, int offset)
{
	struct mvec_header *mh = MBUF2MH(m);
	struct mvec_ent *me = MBUF2ME(m);
	int rem;
	bool owned;

	MPASS(offset <= m->m_pkthdr.len);
	rem = offset;
	if (m->m_ext.ext_flags & EXT_FLAG_EMBREF) {
		owned = (m->m_ext.ext_count == 1);
	} else {
		owned = (*(m->m_ext.ext_cnt) == 1);
	}
	do {
		if (rem > me->me_len) {
			rem -= me->me_len;
			if (owned)
				mvec_ent_free(mh, mh->mh_start);
			mh->mh_start++;
			mh->mh_used--;
			me++;
		} else if (rem < me->me_len) {
			rem = 0;
			me->me_off += rem;
			me->me_len -= rem;
		} else {
			rem = 0;
			mvec_ent_free(mh, mh->mh_start);
			mh->mh_start++;
			mh->mh_used--;
		}
	} while(rem);
	m->m_pkthdr.len -= offset;
	m->m_data = ME_SEG(m, mh, 0);
}

static void
mvec_trim_tail(struct mbuf *m, int offset)
{
	struct mvec_header *mh = MBUF2MH(m);
	struct mvec_ent *me = MBUF2ME(m);
	int i, rem;
	bool owned;

	MPASS(offset <= m->m_pkthdr.len);
	rem = offset;
	if (m->m_ext.ext_flags & EXT_FLAG_EMBREF) {
		owned = (m->m_ext.ext_count == 1);
	} else {
		owned = (*(m->m_ext.ext_cnt) == 1);
	}
	i = mh->mh_count-1;
	me = &me[i];
	do {
		if (rem > me->me_len) {
			rem -= me->me_len;
			me->me_len = 0;
			if (owned)
				mvec_ent_free(mh, i);
			me--;
			mh->mh_used--;
		} else if (rem < me->me_len) {
			rem = 0;
			me->me_len -= rem;
		} else {
			rem = 0;
			me->me_len = 0;
			if (owned)
				mvec_ent_free(mh, i);
			mh->mh_used--;
		}
		i++;
	} while(rem);
	m->m_pkthdr.len -= offset;
}

void
mvec_adj(struct mbuf *m, int req_len)
{
	if (__predict_false(req_len == 0))
		return;
	if (req_len > 0)
		mvec_trim_head(m, req_len);
	else
		mvec_trim_tail(m, req_len);
}

void
mvec_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	panic("%s unimplemented", __func__);
}

static struct mbuf *
mvec_dup_internal(const struct mbuf *m, int how, bool ismvec)
{
	struct mbuf_ext *mextnew;
	struct mvec_header *mhnew;
	struct mvec_ent *menew;
	struct mbuf *mnew;
	const struct mbuf_ext *mext;
	const struct mvec_header *mh;
	const struct mvec_ent *me;
	const struct mbuf *mp;
	caddr_t data;
	int i;

	MBUF_CHECKSLEEP(how);
	if (m == NULL)
		return (NULL);

	if (m->m_pkthdr.len <= PAGE_SIZE - (MSIZE-MVMHLEN)) {
		mextnew = mvec_alloc(1, m->m_pkthdr.len, how);
		mnew = (void *)mextnew;
	} else {
		panic("mvec_dup for > PAGE_SIZE not implemented yet XXX\n");
	}
	if (__predict_false(mnew == NULL))
		return (NULL);

	/* XXX only handle the inline data case */
	menew = mextnew->me_ents;
	mhnew = &mextnew->me_mh;
	mhnew->mh_used = 1;
	memcpy(&mnew->m_pkthdr, &m->m_pkthdr, sizeof(struct pkthdr));
	menew[mhnew->mh_start].me_len = m->m_pkthdr.len;
	menew[mhnew->mh_start].me_type = MVEC_UNMANAGED;
	menew[mhnew->mh_start].me_off = 0;

	data = (void *)(menew + mhnew->mh_count);
	MPASS((caddr_t)(&menew[mhnew->mh_start]) != data);
	mnew->m_data = data;
	menew[mhnew->mh_start].me_cl = data;
	mnew->m_flags |= m->m_flags;
	mnew->m_flags &= ~(M_NOFREE|M_PROTOFLAGS);
	mnew->m_len = 0;

	if (ismvec) {
		mext = (const void *)m;
		mh = &mext->me_mh;
		me = mext->me_ents;
		for (i = mh->mh_start; i < mh->mh_start + mh->mh_used; i++) {
			if (__predict_false(me[i].me_len == 0))
				continue;
			memcpy(mnew->m_data + mnew->m_len, me_data(&me[i]), me[i].me_len);
			mnew->m_len += me[i].me_len;
		}
	} else {
		mp = m;
		do {
			memcpy(mnew->m_data + mnew->m_len, mp->m_data, mp->m_len);
			mnew->m_len += mp->m_len;
			mp = mp->m_next;
		} while (mp != NULL);
	}
	MPASS(mnew->m_len == m->m_pkthdr.len);
	mvec_sanity(mnew);
	return (mnew);
}

struct mbuf *
mvec_dup(const struct mbuf *m, int how)
{
	return (mvec_dup_internal(m, how, m_ismvec(m)));
}

struct mbuf *
mvec_mdup(const struct mbuf *m, int how)
{
	return (mvec_dup_internal(m, how, false));
}

struct mbuf *
mvec_defrag(const struct mbuf *m, int how)
{
	panic("%s unimplemented", __func__);
	return (NULL);
}

struct mbuf *
mvec_collapse(struct mbuf *m, int how, int maxfrags)
{
	panic("%s unimplemented", __func__);
	return (NULL);
}

uint16_t
mvec_cksum_skip(struct mbuf *m, int len, int skip)
{
	u_int64_t sum = 0;
	int mlen = 0;
	int clen = 0;
	caddr_t addr;
	union q_util q_util;
	union l_util l_util;
	struct mvec_cursor mc;
	struct mvec_header mh;
	struct mvec_ent *me;

	MPASS(m_ismvec(m));

	len -= skip;
	mvec_seek(m, &mc, skip);
	mh = *(MBUF2MH(m));

	/* XXX */
	if (mh.mh_multipkt)
		return (0);

	me = MHMEI(m, &mh, mc.mc_idx);
	addr = me->me_cl + me->me_off;
	goto skip_start;

	for (; mh.mh_used && len; me++) {
		mh.mh_used--;
		if (me->me_len == 0)
			continue;
		mlen = me->me_len;
		addr = me->me_cl + me->me_off;
skip_start:
		if (len < mlen)
			mlen = len;
		if ((clen ^ (long) addr) & 1)
		    sum += in_cksumdata(addr, mlen) << 8;
		else
		    sum += in_cksumdata(addr, mlen);

		clen += mlen;
		len -= mlen;
	}
	REDUCE16;
	return (~sum & 0xffff);
}

struct mbuf *
mvec_prepend(struct mbuf *m, int size)
{
	struct mvec_header *mh;
	struct mvec_ent *me;
	struct mbuf *data;
	struct mbuf_ext *mext;

	MPASS(size <= MSIZE);
	if (__predict_false((data = m_get(M_NOWAIT, MT_NOINIT)) == NULL))
		return (NULL);

	mext = (struct mbuf_ext *)m;
	mh = &mext->me_mh;
	if (__predict_true(mh->mh_start)) {
		mh->mh_start--;
		mh->mh_used++;
		me = MHMEI(m, mh, 0);
		me->me_len = size;
		me->me_cl = (caddr_t)data;
		me->me_off = 0;
		me->me_type = MVEC_MBUF;
		me->me_eop = 0;
		me->me_ext_flags = 0;
		me->me_ext_type = EXT_MBUF;
		m->m_pkthdr.len += size;
		m->m_len = size;
		m->m_data = me->me_cl;
	} else {
		panic("implement fallback path for %s", __func__);
	}
	return (m);
}

struct mbuf *
mvec_append(struct mbuf *m, caddr_t cl, uint16_t off,
						 uint16_t len, uint8_t cltype)
{
	struct mvec_header *mh;
	struct mvec_ent *me;

	mh = MBUF2MH(m);
	KASSERT(mh->mh_used < mh->mh_count,
			("need to add support for growing mvec on append"));
	me = MHMEI(m, mh, mh->mh_used);
	me->me_cl = cl;
	me->me_off = off;
	me->me_len = len;
	me->me_ext_type = cltype;
	if (cltype == 0)
		me->me_type = MVEC_UNMANAGED;
	me->me_ext_flags = 0;
	m->m_pkthdr.len += len;
	if (mh->mh_used == 0) {
		m->m_len = len;
		m->m_data = (cl + off);
	}
	mh->mh_used++;
	return (m);
}

static int
mvec_init_mbuf_(struct mbuf *m, uint8_t count, uint8_t type, int len)
{
	struct mbuf_ext *mext;
	struct mvec_ent *me;
	struct mvec_header *mh;
	int rc;

	mext = (void *)m;
	mh = &mext->me_mh;
	me = mext->me_ents;
	*((uint64_t *)mh) = 0;
	if (type == MVALLOC_MBUF) {
		if (len == 0) {
			mh->mh_count = MBUF_ME_MAX;
			/* leave room for prepend */
			mh->mh_start = 1;
		} else {
			mh->mh_count = count+1;
			mh->mh_start = 0;
		}
	} else {
		mh->mh_count = count+1;
		mh->mh_start = 1;
	}
	bzero(me, sizeof(*me)*mh->mh_count);
	mh->mh_mvtype = type;

	rc = m_init(m, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (__predict_false(rc))
		return (rc);

	m->m_next = m->m_nextpkt = NULL;
	m->m_len = 0;
	m->m_data = NULL;
	m->m_flags = M_PKTHDR|M_EXT;
	m->m_ext.ext_free = NULL;
	m->m_ext.ext_arg1 = m->m_ext.ext_arg2 = NULL;
	m->m_ext.ext_flags = EXT_FLAG_EMBREF;
	m->m_ext.ext_type = EXT_MVEC;
	m->m_ext.ext_size = MSIZE;
	m->m_ext.ext_buf = (caddr_t)m;
	m->m_ext.ext_cnt = NULL;
	m->m_ext.ext_count = 1;
	return (0);
}

int
mvec_init_mbuf(struct mbuf *m, uint8_t count, uint8_t type)
{

	return (mvec_init_mbuf_(m, count, type, 0));
}

struct mbuf_ext *
mvec_alloc(uint8_t count, int len, int how)
{
	int size;
	uint8_t type;
	struct mbuf_ext *m;

	size = sizeof(*m) + (count + 1)*sizeof(struct mvec_ent);
	size += len;
	if (size <= MVMHLEN) {
		m = (void*)m_get(how, MT_NOINIT);
		type = MVALLOC_MBUF;
	} else if (size > (1024-(MSIZE-MVMHLEN)) && size <= MVMHCLLEN) {
		m = (void *)uma_zalloc(zone_clust, how);
		type = MVALLOC_CLUSTER;
	} else {
		m = malloc(size, M_MVEC, how);
		type = MVALLOC_MALLOC;
	}
	if (__predict_false(m == NULL))
		return (NULL);
	mvec_init_mbuf_((struct mbuf *)m, count, type, len);
	return (m);
}

static int
mvec_ent_size(struct mvec_ent *me)
{
	int type;

	MPASS(me->me_ext_type && (me->me_ext_type < 32));

	type = me->me_ext_type;
	MPASS((1<<type) & validtypes);
	return (type2len[type]);
}

struct mbuf *
mvec_pullup(struct mbuf *m, int idx, int count)
{
	struct mvec_header *mh;
	struct mvec_ent *mecur, *menxt;
	int tailroom, size, copylen, doff, i, len;

	/* XXX --- fix */
	MPASS(idx == 0);
	mvec_sanity(m);
	MPASS(count <= m->m_pkthdr.len);
	mh = MBUF2MH(m);
	mecur = MHMEI(m, mh, 0);
	size = mvec_ent_size(mecur);
	tailroom = size - mecur->me_off - mecur->me_len;
	MPASS(tailroom >= 0);
	copylen = count - mecur->me_len;

	if (__predict_false(count <= mecur->me_len))
		return (m);
	/*
	 * XXX - If we're not the exclusive owner we need to allocate a new
	 * buffer regardless.
	 */
	if (copylen > size) {
		/* allocate new buffer */
		panic("allocate new buffer copylen=%d size=%d", copylen, size);
	} else if (copylen > tailroom) {
		/*
		 * move data up if possible
		 * else allocate new buffer
		 */
		panic("relocate data copylen=%d size=%d tailroom=%d", copylen, size, tailroom);
	}
	doff = mecur->me_off + mecur->me_len;
	i = 1;
	do {
		menxt = MHMEI(m, mh, i);
		len = min(copylen, menxt->me_len);
		bcopy(ME_SEG(m, mh, i), mecur->me_cl + doff, len);
		doff += len;
		mecur->me_len += len;
		menxt->me_off += len;
		menxt->me_len -= len;
		copylen -= len;
		i++;
	} while (copylen);
	m->m_data = ME_SEG(m, mh, 0);
	m->m_len = ME_LEN(m, mh, 0);
	mvec_sanity(m);
	return (m);
}

void
mvec_free(struct mbuf_ext *m)
{
	struct mvec_header *mh;
	struct mvec_ent *me;
	m_refcnt_t *me_count;
	int i;

	mvec_sanity((void *)m);
	mh = &m->me_mh;
	me = m->me_ents;
	me_count = (m_refcnt_t *)(me + mh->mh_count);

	for (i = 0; i < mh->mh_count; i++, me_count++, me++) {
		if (__predict_false(me->me_cl == NULL))
			continue;
		switch (me->me_type) {
			case MVEC_MBUF:
				uma_zfree_arg(zone_mbuf, me->me_cl, (void *)MB_DTOR_SKIP);
				break;
			case MVEC_MANAGED:
				mvec_clfree(me, me_count, mh->mh_multiref);
				break;
			default:
				/* ... */
				break;
		}
#ifdef INVARIANTS
		me->me_cl = (void*)0xdeadbeef;
#endif
	}
	mvec_buffer_free((void*)m);
}

struct mbuf_ext *
mchain_to_mvec(struct mbuf *m, int how)
{
	struct mbuf *mp, *mnext;
	struct mbuf_ext *mnew;
	struct mvec_header *mh;
	struct mvec_ent *me;
	int i, count, size;
	bool dupref;
	m_refcnt_t *me_count;

	if (__predict_false(m_ismvec(m)))
		return ((struct mbuf_ext *)m);

	size = count = 0;
	dupref = false;
	me_count = NULL;
	for (mp = m; mp != NULL; mp = mnext) {
		mnext = mp->m_next;
		count++;
		if (!(mp->m_flags & M_EXT))
			continue;

		/*
		 * bail on ext_free -- we can't efficiently pass an mbuf
		 * at free time and m_ext adds up to a lot of space
		 */
		if (mp->m_ext.ext_free != NULL) {
			DPRINTF("%s ext_free is set: %p\n", __func__, mp->m_ext.ext_free);
			return (NULL);
		}
	    dupref = ((mp->m_ext.ext_flags & EXT_FLAG_EMBREF) && (mp->m_ext.ext_count > 1)) ||
			(!(mp->m_ext.ext_flags & EXT_FLAG_EMBREF) && (*(mp->m_ext.ext_cnt) > 1));
	}

	/* add spare */
	count++;
	if (dupref)
		size = count*sizeof(void*);
	mnew = mvec_alloc(count, size, how);

	if (mnew == NULL) {
		DPRINTF("%s malloc failed\n", __func__);
		return (NULL);
	}
	mh = &mnew->me_mh;
	mh->mh_used = count-1;
	MPASS(mh->mh_start == 1);
#ifdef INVARIANTS
	if (size)
		MPASS(mh->mh_count == mh->mh_used+1);
	else
		MPASS(mh->mh_count >= mh->mh_used);
#endif
	mh->mh_multiref = dupref;
	/* leave first entry open for encap */
	bcopy(&m->m_pkthdr, &mnew->me_mbuf.m_pkthdr, sizeof(struct pkthdr));

	me = mnew->me_ents;
	me->me_cl = NULL;
	me->me_off = me->me_len = 0;
	me->me_ext_type = me->me_ext_flags = 0;
	if (dupref) {
		me_count = MBUF2REF(mnew);
		MPASS(me_count == (void*)(mnew->me_ents + mnew->me_mh.mh_count));
		bzero(me_count, count*sizeof(void *));
		me_count++;
	}
	me++;
	for (i = 0, mp = m; mp != NULL; mp = mnext, me++, me_count++, i++) {
		mnext = mp->m_next;
		me->me_len = mp->m_len;
		if (mp->m_flags & M_EXT) {
			me->me_cl = mp->m_ext.ext_buf;
			me->me_off = ((uintptr_t)mp->m_data - (uintptr_t)mp->m_ext.ext_buf);
			me->me_type = MVEC_MANAGED;
			me->me_ext_flags = mp->m_ext.ext_flags;
			MPASS(mp->m_ext.ext_type < 32);
			me->me_ext_type = mp->m_ext.ext_type;
#ifdef INVARIANTS
			(void)mvec_ent_size(me);
#endif			
			if (dupref) {
				if (mp->m_ext.ext_flags & EXT_FLAG_EMBREF) {
					me_count->ext_cnt = &mp->m_ext.ext_count;
					me->me_ext_flags &= ~EXT_FLAG_EMBREF;
				} else {
					me_count->ext_cnt = mp->m_ext.ext_cnt;
					if (!(mp->m_flags & M_NOFREE))
						uma_zfree_arg(zone_mbuf, mp, (void *)MB_DTOR_SKIP);
				}
				DPRINTF("setting me_count: %p i: %d to me_count->ext_cnt: %p\n",
					   me_count, i, me_count->ext_cnt);
			}
		} else {
			me->me_cl = (caddr_t)mp;
			me->me_off = ((uintptr_t)(mp->m_data) - (uintptr_t)mp);
			me->me_type = MVEC_MBUF;
			me->me_ext_flags = 0;
			me->me_ext_type = EXT_MBUF;
			if (mp->m_flags & M_NOFREE)
				me->me_ext_flags |= EXT_FLAG_NOFREE;
		}
		me->me_eop = 0;
	}
	mnew->me_mbuf.m_len = mnew->me_ents[1].me_len;
	mnew->me_mbuf.m_data = (mnew->me_ents[1].me_cl + mnew->me_ents[1].me_off);
	mh = MBUF2MH(mnew);
	MPASS(mh->mh_count >= mh->mh_start + mh->mh_used);
	mvec_sanity((void*)mnew);
	return (mnew);
}

struct mbuf_ext *
pktchain_to_mvec(struct mbuf *m, int mtu, int how)
{
	struct mbuf *mp, *mnext;
	struct mbuf_ext *mnew, *mh, *mt;

	mp = m;
	mh = mt = NULL;
	while (mp) {
		mnext = mp->m_nextpkt;
		if (mp->m_pkthdr.csum_flags & CSUM_TSO) {
			mnew = mchain_to_mvec(mp, how);
		} else  {
			MPASS(mp->m_pkthdr.len <= mtu + 14);
			mnew = (void*)mp;
		}
		if (__predict_false(mnew == NULL)) {
			m_freem(mp);
			mp = mnext;
			continue;
		}
		if (mh == NULL) {
			mh = mt = mnew;
		} else {
			mt->me_mbuf.m_nextpkt = (void*)mnew;
			mt = mnew;
		}
		mp = mnext;
	}
	return (mh);
}

static void
m_ext_init(struct mbuf *m, struct mbuf_ext *head, struct mvec_header *mh)
{
	struct mvec_ent *me;
	struct mbuf *headm;
	bool doref;

	headm = &head->me_mbuf;
	doref = true;
	me = &head->me_ents[mh->mh_start];
	m->m_ext.ext_buf = me->me_cl;
	m->m_ext.ext_arg1 = headm->m_ext.ext_arg1;
	m->m_ext.ext_arg2 = headm->m_ext.ext_arg2;
	m->m_ext.ext_free = headm->m_ext.ext_free;
	m->m_ext.ext_type = me->me_ext_type;
	if (me->me_ext_type) {
		m->m_ext.ext_flags = me->me_ext_flags;
		m->m_ext.ext_size = mvec_ent_size(me);
	} else {
		m->m_ext.ext_flags = EXT_FLAG_NOFREE;
		/* Only used by m_sanity so just call it our size */
		m->m_ext.ext_size = me->me_len + me->me_off;
	}
	/*
	 * There are 2 cases for refcount transfer:
	 *  1) all clusters are owned by the mvec [default]
	 *     - point at mvec refcnt and increment
	 *  2) cluster has a normal external refcount
	 */
	if (__predict_true(!head->me_mh.mh_multiref)) {
		m->m_ext.ext_flags = EXT_FLAG_MVECREF;
		if (headm->m_ext.ext_flags & EXT_FLAG_EMBREF)
			m->m_ext.ext_cnt = &headm->m_ext.ext_count;
		else
			m->m_ext.ext_cnt = headm->m_ext.ext_cnt;
	} else {
		m_refcnt_t *ref = MHREFI(headm, mh, 0);

		m->m_ext.ext_cnt = ref->ext_cnt;
		if (ref->ext_cnt == NULL) {
			m->m_ext.ext_flags |= EXT_FLAG_EMBREF;
			m->m_ext.ext_type = 0;
			m->m_ext.ext_count = 1;
			doref = false;
		}
	}
	if (doref)
		atomic_add_int(m->m_ext.ext_cnt, 1);
}

static struct mbuf *
mvec_to_mchain_pkt(struct mbuf_ext *mp, struct mvec_header *mhdr, int how)
{
	struct mvec_ent *me;
	struct mbuf *m, *mh, *mt, *mpm;

	if (__predict_false((mh = m_gethdr(how, MT_DATA)) == NULL))
		return (NULL);

	mpm = &mp->me_mbuf;
	me = MHMEI(mp, mhdr, 0);
	mh->m_flags |= M_EXT;
	mh->m_flags |= mpm->m_flags & (M_BCAST|M_MCAST|M_PROMISC|M_VLANTAG|M_VXLANTAG);
	/* XXX update csum_data after encap */
	mh->m_pkthdr.csum_data = mpm->m_pkthdr.csum_data;
	mh->m_pkthdr.csum_flags = mpm->m_pkthdr.csum_flags;
	mh->m_pkthdr.vxlanid = mpm->m_pkthdr.vxlanid;
	m_ext_init(mh, mp, mhdr);
	mh->m_data = me->me_cl + me->me_off;
	mh->m_pkthdr.len = mh->m_len = me->me_len;
	mhdr->mh_start++;
	mhdr->mh_used--;
	mt = mh;
	while (!me->me_eop && mhdr->mh_used) {
		if (__predict_false((m = m_get(how, MT_DATA)) == NULL))
			goto fail;
		me++;
		mt->m_next = m;
		mt = m;
		mt->m_flags |= M_EXT;
		m_ext_init(mt, mp, mhdr);
		mt->m_len = me->me_len;
		mh->m_pkthdr.len += mt->m_len;
		mt->m_data = me->me_cl + me->me_off;
		mhdr->mh_start++;
		mhdr->mh_used--;
	}
#ifdef INVARIANTS
	m_sanity(mh, 0);
#endif
	return (mh);
 fail:
	if (mh)
		m_freem(mh);
	return (NULL);
}

struct mbuf *
mvec_to_mchain(struct mbuf *mp, int how)
{
	struct mvec_header *pmhdr, mhdr;
	struct mbuf *mh, *mt, *m;
#ifdef INVARIANTS
	int count = 0;
#endif

	mvec_sanity(mp);
	pmhdr = MBUF2MH(mp);
	bcopy(pmhdr, &mhdr, sizeof(mhdr));
	mh = mt = NULL;
	while (mhdr.mh_used) {
#ifdef INVARIANTS
		count++;
#endif
		if (__predict_false((m = mvec_to_mchain_pkt((struct mbuf_ext *)mp, &mhdr, how)) == NULL)) {
			DPRINTF("mvec_to_mchain_pkt failed\n");
			goto fail;
		}
		if (mh != NULL) {
			mt->m_nextpkt = m;
			mt = m;
		} else
			mh = mt = m;
	}
#ifdef INVARIANTS
	m = mh;
	while (m) {
		MPASS(m->m_data);
		m_sanity(m, 0);
		m = m->m_nextpkt;
		count--;
	}
	MPASS(count == 0);
#endif
	return (mh);
 fail:
	m_freechain(mh);
	return (NULL);
}

/*
 * Move the below to net/ once working 
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/iflib.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <machine/in_cksum.h>

#define MIN_HDR_LEN   (ETHER_HDR_LEN + sizeof(struct ip) + sizeof(struct tcphdr))

static int
mvec_parse_header(struct mbuf_ext *mp, int prehdrlen, if_pkt_info_t pi)
{
	struct ether_vlan_header *evh;
	struct mvec_header *mh = &mp->me_mh;
	struct mbuf *m;

	m = (void*)mp;
	mvec_sanity(m);
	if (__predict_false(m->m_len < MIN_HDR_LEN + prehdrlen) &&
		__predict_false(mvec_pullup(m, 0, prehdrlen + MIN_HDR_LEN) == NULL))
			return (ENOMEM);
	evh = (struct ether_vlan_header *)(ME_SEG(m, mh, 0) + prehdrlen);
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		pi->ipi_etype = ntohs(evh->evl_proto);
		pi->ipi_ehdrlen = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
	} else {
		pi->ipi_etype = ntohs(evh->evl_encap_proto);
		pi->ipi_ehdrlen = ETHER_HDR_LEN;
	}
	switch (pi->ipi_etype) {
		case ETHERTYPE_IP: {
			struct ip *ip = NULL;
			struct tcphdr *th = NULL;
			int minthlen;

			minthlen = pi->ipi_ehdrlen + sizeof(*ip) + sizeof(*th);
			if (__predict_false(m->m_len < minthlen + prehdrlen) &&
				__predict_false(mvec_pullup(m, 0, prehdrlen + minthlen) == NULL))
				return (ENOMEM);
			ip = (struct ip *)(ME_SEG(m, mh, 0) + prehdrlen + pi->ipi_ehdrlen);
			pi->ipi_ip_hlen = ip->ip_hl << 2;
			pi->ipi_ipproto = ip->ip_p;
			if (ip->ip_p != IPPROTO_TCP)
				return (EINVAL);
			minthlen = pi->ipi_ehdrlen + pi->ipi_ip_hlen + sizeof(*th);
			if (__predict_false(m->m_len < minthlen + prehdrlen) &&
				__predict_false(mvec_pullup(m, 0, prehdrlen + minthlen) == NULL))
				return (ENOMEM);
			th = (struct tcphdr *)(ME_SEG(m, mh, 0) + prehdrlen + pi->ipi_ehdrlen + pi->ipi_ip_hlen);
			pi->ipi_tcp_hflags = th->th_flags;
			pi->ipi_tcp_hlen = th->th_off << 2;
			pi->ipi_tcp_seq = th->th_seq;
			minthlen = pi->ipi_ehdrlen + pi->ipi_ip_hlen + pi->ipi_tcp_hlen;
			if (__predict_false(m->m_len < minthlen + prehdrlen) &&
				__predict_false(mvec_pullup(m, 0, prehdrlen + minthlen) == NULL))
				return (ENOMEM);
			break;
		}
		case ETHERTYPE_IPV6: {
			break;
		}
		default:
			/* XXX unsupported -- error */
			break;
	}
	mvec_sanity(m);
	return (0);
}

enum tso_seg_type {
	TSO_FIRST,
	TSO_MIDDLE,
	TSO_LAST
};

struct tso_state {
	if_pkt_info_t ts_pi;
	tcp_seq ts_seq;
	uint16_t ts_idx;
	uint16_t ts_prehdrlen;
	uint16_t ts_hdrlen;
	uint16_t ts_segsz;
};

static void
tso_fixup(struct tso_state *state, caddr_t hdr, int len, enum tso_seg_type type)
{
	if_pkt_info_t pi = state->ts_pi;
	struct ip *ip;
	struct udphdr *uh;
	struct tcphdr *th;
	uint16_t encap_len, plen;

	if (state->ts_prehdrlen &&
		((type == TSO_FIRST) || (len != state->ts_segsz))) {
		ip = (struct ip *)(hdr + ETHER_HDR_LEN);
		plen = len + state->ts_hdrlen - ETHER_HDR_LEN;
		ip->ip_len = htons(plen);
		ip->ip_sum = 0;
		ip->ip_sum = in_cksum_hdr(ip);
		uh = (struct udphdr *)(ip + 1);
		plen -= sizeof(*ip);
		uh->uh_ulen = htons(plen);
		uh->uh_sum = 0;
		uh->uh_sum = in_pseudo(ip->ip_src.s_addr, ip->ip_dst.s_addr, htons(IPPROTO_UDP + plen));
	}
	encap_len = len + state->ts_hdrlen - state->ts_prehdrlen - pi->ipi_ehdrlen;
	if (pi->ipi_etype == ETHERTYPE_IP) {
		ip = (struct ip *)(hdr + state->ts_prehdrlen + pi->ipi_ehdrlen);
		if ((type == TSO_FIRST) || (len != state->ts_segsz)) {
			ip->ip_ttl = 255;
			ip->ip_len = htons(encap_len);
			ip->ip_sum = 0;
			ip->ip_sum = in_cksum_hdr(ip);
		}
	} else if (pi->ipi_etype == ETHERTYPE_IPV6) {
		/* XXX notyet */
	} else {
		panic("bad ethertype %d in tso_fixup", pi->ipi_etype);
	}
	if (pi->ipi_ipproto == IPPROTO_TCP) {
		th = (struct tcphdr *)(hdr + state->ts_prehdrlen + pi->ipi_ehdrlen + pi->ipi_ip_hlen);
		th->th_seq = htonl(state->ts_seq);
		state->ts_seq += len;
		plen = len - pi->ipi_ehdrlen - pi->ipi_ip_hlen;
		th->th_sum = 0;
		/* Zero the PSH and FIN TCP flags if this is not the last
		   segment. */
		if (type != TSO_LAST)
			th->th_flags &= ~(0x8 | 0x1);
	} else {
		panic("non TCP IPPROTO %d in tso_fixup", pi->ipi_ipproto);
	}
}

static void
tso_init(struct tso_state *state, caddr_t hdr, if_pkt_info_t pi, int prehdrlen, int hdrlen, int segsz)
{
	struct ip *ip;
	struct tcphdr *th;

	MPASS(hdrlen > prehdrlen);
	ip = (struct ip *)(hdr + prehdrlen + pi->ipi_ehdrlen);
	th = (struct tcphdr *)(ip + 1);
	state->ts_pi = pi;
	state->ts_idx = ntohs(ip->ip_id);
	state->ts_prehdrlen = prehdrlen;
	state->ts_hdrlen = hdrlen;
	state->ts_seq = ntohl(th->th_seq);
	state->ts_segsz = segsz;
	/* XXX assuming !VLAN */
	tso_fixup(state, hdr, segsz, TSO_FIRST);
}

struct mbuf_ext *
mvec_tso(struct mbuf_ext *mprev, int prehdrlen, bool freesrc)
{
	struct mvec_header *mh, *newmh;
	struct mvec_cursor mc;
	struct mvec_ent *me, *mesrc, *medst, *newme;
	struct mbuf_ext *mnew;
	struct mbuf *m;
	struct if_pkt_info pi;
	struct tso_state state;
	m_refcnt_t *newme_count, *medst_count, *mesrc_count;
	int segcount, soff, segrem, srem;
	int i, segsz, nheaders, hdrsize;
	int refsize, count, pktrem, srci, dsti;
	volatile uint32_t *refcnt;
	bool dupref, dofree, sop;
	caddr_t hdrbuf;

	m = (void*)mprev;
	mvec_sanity(m);
	dofree = false;
	if (m->m_ext.ext_flags & EXT_FLAG_EMBREF) {
		refcnt = &m->m_ext.ext_count;
	} else {
		refcnt = m->m_ext.ext_cnt;
	}
	if (freesrc && (*refcnt == 1))
		dofree = true;

	mh = &mprev->me_mh;
	me = mprev->me_ents;
	dupref = mh->mh_multiref;
	if (mvec_parse_header(mprev, prehdrlen, &pi))
		return (NULL);
	if (m->m_pkthdr.tso_segsz)
		segsz = m->m_pkthdr.tso_segsz;
	else
		segsz = m->m_pkthdr.rcvif->if_mtu - pi.ipi_ehdrlen + pi.ipi_ip_hlen + pi.ipi_tcp_hlen;
	hdrsize = prehdrlen + pi.ipi_ehdrlen + pi.ipi_ip_hlen + pi.ipi_tcp_hlen;
	pktrem = m->m_pkthdr.len - hdrsize;
	nheaders = pktrem / segsz;
	if (nheaders*segsz != pktrem)
		nheaders++;
	segrem = segsz;
	segcount = refsize = 0;
	mvec_seek(m, &mc, hdrsize);
	soff = mc.mc_off;
	srci = mc.mc_idx;
	while (pktrem > 0) {
		MPASS(pktrem >= segrem);
		MPASS(srci < mprev->me_mh.mh_count);
		if (__predict_false(me[srci].me_len == 0)) {
			srci++;
			continue;
		}
		segrem = min(pktrem, segsz);
		do {
			int used;

			srem = me[srci].me_len - soff;
			used = min(segrem, srem);
			srem -= used;
			if (srem) {
				soff += segrem;
			} else {
				srci++;
				soff = 0;
			}
			segrem -= used;
			pktrem -= used;
			segcount++;
		} while (segrem);
	}

	count = segcount + nheaders;
	if (mh->mh_multiref)
		refsize = count*sizeof(void*);

	mnew = mvec_alloc(count, refsize + (nheaders * hdrsize), M_NOWAIT);
	if (__predict_false(mnew == NULL))
		return (NULL);
	bcopy(&m->m_pkthdr, &mnew->me_mbuf.m_pkthdr, sizeof(struct pkthdr) + sizeof(struct m_ext));
	newmh = &mnew->me_mh;
	newmh->mh_start = 0;
	newmh->mh_used = count;
	newmh->mh_multiref = mh->mh_multiref;
	newmh->mh_multipkt = true;
	newme = mnew->me_ents;
	newme_count = MBUF2REF(mnew);
	__builtin_prefetch(newme_count);
	medst_count = newme_count;
	medst = newme;

	/*
	 * skip past header info
	 */
	mvec_seek(m, &mc, hdrsize);
	mesrc = mprev->me_ents;
	mesrc_count = MBUF2REF(m);
	if (dupref)
		bzero(medst_count, count*sizeof(void *));
	/*
	 * Packet segmentation loop
	 */
	srci = mc.mc_idx;
	soff = mc.mc_off;
	pktrem = m->m_pkthdr.len - hdrsize;
	sop = true;
	hdrbuf = ((caddr_t)(newme + count)) + refsize;
	/*
	 * Replicate input header nheaders times
	 * and update along the way
	 */
	bcopy(me_data(mesrc), hdrbuf, hdrsize);
	tso_init(&state, hdrbuf, &pi, prehdrlen, hdrsize, segsz);
	for (i = 1; i < nheaders; i++) {
		MPASS(pktrem > 0);
		bcopy(hdrbuf, hdrbuf + (i*hdrsize), hdrsize);
		tso_fixup(&state, hdrbuf + (i*hdrsize), min(pktrem, segsz),
				  (pktrem <= segsz) ? TSO_LAST : TSO_MIDDLE);
		pktrem -= segsz;
	}
	pktrem = m->m_pkthdr.len - hdrsize;
	for (dsti = i = 0; i < nheaders; i++) {
		int used;

		medst[dsti].me_cl = hdrbuf;
		medst[dsti].me_len = hdrsize;
		medst[dsti].me_off = i*hdrsize;
		medst[dsti].me_type = MVEC_UNMANAGED;
		dsti++;

		MPASS(pktrem > 0);
		for (used = 0, segrem = min(segsz, pktrem); segrem; dsti++) {
			MPASS(pktrem > 0);
			MPASS(srci < mprev->me_mh.mh_count);
			MPASS(dsti < mnew->me_mh.mh_count);
			/*
			 * Skip past any empty slots
			 */
			while (mesrc[srci].me_len == 0)
				srci++;
			/*
			 * At the start of a source descriptor:
			 * copy its attributes and, if dupref,
			 * its refcnt
			 */
			if (soff == 0 || sop) {
				if (dupref) {
					DPRINTF("dsti: %d srci: %d sop: %d soff: %d --- setting %p to %p\n",
						   dsti, srci, sop, soff, &medst_count[dsti], mesrc_count[srci].ext_cnt);
					medst_count[dsti].ext_cnt = mesrc_count[srci].ext_cnt;
					if (!dofree && (mesrc_count[srci].ext_cnt != NULL))
						atomic_add_int(mesrc_count[srci].ext_cnt, 1);
				}
				medst[dsti].me_type = mesrc[srci].me_type;
				medst[dsti].me_ext_flags = mesrc[srci].me_ext_flags;
				medst[dsti].me_ext_type = mesrc[srci].me_ext_type;
				sop = false;
			} else {
				medst[dsti].me_type = MVEC_UNMANAGED;
				medst[dsti].me_ext_flags = 0;
				medst[dsti].me_ext_type = 0;
			}
			/*
			 * Remaining value is len - off
			 */
			srem = mesrc[srci].me_len - soff;
			medst[dsti].me_cl = mesrc[srci].me_cl;
			medst[dsti].me_off = mesrc[srci].me_off + soff;
			used = min(segrem, srem);
			srem -= used;
			if (srem) {
				soff += segrem;
			} else {
				srci++;
				soff = 0;
			}
			segrem -= used;
			pktrem -= used;
			medst[dsti].me_eop = (segrem == 0);
			medst[dsti].me_len = used;
		}
	}
	MPASS(dsti == mnew->me_mh.mh_count);
	if (mprev->me_mh.mh_multiref)
		MPASS(srci == mprev->me_mh.mh_count);
	else
		MPASS(srci <= mprev->me_mh.mh_count);
	mnew->me_mbuf.m_len = mnew->me_ents->me_len;
	mnew->me_mbuf.m_data = (mnew->me_ents->me_cl + mnew->me_ents->me_off);
	mnew->me_mbuf.m_pkthdr.len = m->m_pkthdr.len + (nheaders - 1)*hdrsize;
	mvec_sanity((struct mbuf *)mnew);
	m->m_flags |= M_PROTO1;

	if (dofree) {
		if (mesrc->me_cl && (mesrc->me_type == MVEC_MBUF) && mesrc->me_len == hdrsize)
			uma_zfree_arg(zone_mbuf, mesrc->me_cl, (void *)MB_DTOR_SKIP);
		mnew->me_mbuf.m_ext.ext_count = 1;
		if (!(m->m_ext.ext_flags & EXT_FLAG_EMBREF))
			mvec_buffer_free(__containerof(refcnt, struct mbuf, m_ext.ext_count));
		/* XXX we're leaking here */
		mvec_buffer_free(m);
	} else {
		if (m->m_ext.ext_flags & EXT_FLAG_EMBREF)
			mnew->me_mbuf.m_ext.ext_cnt = m->m_ext.ext_cnt;
		else
			mnew->me_mbuf.m_ext.ext_cnt = &m->m_ext.ext_count;
		atomic_add_int(mnew->me_mbuf.m_ext.ext_cnt, 1);
	}
	return (mnew);
}
