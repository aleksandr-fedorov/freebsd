/*-
 * Copyright (c) 2018 Leon Dang
 * Copyright (c) 2017 Shunsuke Mie
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

/*
 * bhyve PCIe-NVMe device emulation.
 *
 * options:
 *  -s <n>,nvme,devpath,maxq=#,qsz=#,ioslots=#,sectsz=#,ser=A-Z
 *
 *  accepted devpath:
 *    /dev/blockdev
 *    /path/to/image
 *    ram=size_in_MB
 *
 *  maxq    = max number of queues
 *  qsz     = max elements in each queue
 *  ioslots = max number of concurrent io requests
 *  sectsz  = sector size (defaults to blockif sector size)
 *  ser     = serial number (20-chars max)
 *
 */

/* TODO:
    - create async event for smart and log
    - intr coalesce
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>

#include <machine/vmm.h>
#include <machine/atomic.h>
#include <vmmapi.h>

#include <dev/nvme/nvme.h>

#include "pci_emul.h"
#include "block_if.h"
#include "bhyverun.h"


static int nvme_debug = 0;
#define	DPRINTF(params) if (nvme_debug) printf params
#define	WPRINTF(params) printf params

/* defaults; can be overridden */
#define	NVME_MSIX_BAR		4

#define NVME_IOSLOTS		8

#define NVME_QUEUES		16
#define NVME_MAX_QENTRIES	2048

#define NVME_PRP2_ITEMS		(PAGE_SIZE/sizeof(uint64_t))
#define NVME_MAX_BLOCKIOVS	512

/* helpers */

#define NVME_DOORBELL_OFFSET offsetof(struct nvme_registers, doorbell)

enum nvme_controller_register_offsets {
 	NVME_CR_CAP_LOW = 0x00,
	NVME_CR_CAP_HI  = 0x04,
	NVME_CR_VS      = 0x08,
	NVME_CR_INTMS   = 0x0c,
	NVME_CR_INTMC   = 0x10,
	NVME_CR_CC      = 0x14,
	NVME_CR_CSTS    = 0x1c,
	NVME_CR_NSSR    = 0x20,
	NVME_CR_AQA     = 0x24,
	NVME_CR_ASQ_LOW = 0x28,
	NVME_CR_ASQ_HI  = 0x2c,
	NVME_CR_ACQ_LOW = 0x30,
	NVME_CR_ACQ_HI  = 0x34,
};

enum nvme_cmd_cdw11 {
	NVME_CMD_CDW11_PC  = 0x0001,
	NVME_CMD_CDW11_IEN = 0x0002,
	NVME_CMD_CDW11_IV  = 0xFFFF0000,
};


#define NVME_CQ_INTEN   0x01
#define NVME_CQ_INTCOAL 0x02

struct nvme_completion_queue {
	struct nvme_completion *qbase;
	uint32_t	size;
	uint16_t	tail; /* nvme progress */
	uint16_t	head; /* guest progress */
	uint16_t	intr_vec;
	uint32_t	intr_en;
	pthread_mutex_t	mtx;
};

struct nvme_submission_queue {
	struct nvme_command *qbase;
	uint32_t	size;
	uint16_t	head; /* nvme progress */
	uint16_t	tail; /* guest progress */
	uint16_t	cqid; /* completion queue id */
	int		busy; /* queue is being processed */
	int		qpriority;
};

enum nvme_storage_type {
	NVME_STOR_BLOCKIF = 0,
	NVME_STOR_RAM = 1,
};

struct pci_nvme_blockstore {
	enum nvme_storage_type type;
	void		*ctx;
	uint64_t	size;
	uint32_t	sectsz;
	uint32_t	sectsz_bits;
};

struct pci_nvme_ioreq {
	struct pci_nvme_softc *sc;
	struct pci_nvme_ioreq *next;
	struct nvme_submission_queue *nvme_sq;
	uint16_t sqid;

	/* command information */
	uint16_t opc;
	uint16_t cid;
	uint32_t nsid;

	uint64_t prev_gpaddr;
	size_t   prev_size;

	/*
	 * lock if all iovs consumed (big IO);
	 * complete transaction before continuing
	 */
	pthread_mutex_t mtx;
	pthread_cond_t  cv;

	struct blockif_req io_req;

	/* pad to fit up to 512 page descriptors from guest IO request */
	struct iovec	iovpadding[NVME_MAX_BLOCKIOVS-BLOCKIF_IOV_MAX];
};

struct pci_nvme_softc {
	struct pci_devinst *nsc_pi;

	pthread_mutex_t	mtx;

	struct nvme_registers regs;

	struct nvme_namespace_data  nsdata;
	struct nvme_controller_data ctrldata;

	struct pci_nvme_blockstore nvstore;

	uint16_t	max_qentries; /* max entries per queue */
	uint32_t	max_queues;
	uint32_t	num_cqueues;
	uint32_t	num_squeues;

	struct pci_nvme_ioreq *ioreqs;
	struct pci_nvme_ioreq *ioreqs_free; /* free list of ioreqs */
	uint32_t	pending_ios;
	uint32_t	ioslots;
	sem_t		iosemlock;

	/* status and guest memory mapped queues */
	struct nvme_completion_queue *compl_queues;
	struct nvme_submission_queue *submit_queues;

	/* controller features */
	uint32_t	intr_coales_aggr_time;   /* 0x08: uS to delay intr */
	uint32_t	intr_coales_aggr_thresh; /* 0x08: compl-Q entries */
	uint32_t	async_ev_config;         /* 0x0B: async event config */
};


static void pci_nvme_io_partial(struct blockif_req *br, int err);

static void
pci_nvme_init_ctrldata(struct pci_nvme_softc *sc)
{
	struct nvme_controller_data *cd = &sc->ctrldata;

	cd->vid = 0xFB5D;
	cd->ssvid = 0x0000;

	cd->mn[0] = 'b';
	cd->mn[1] = 'h';
	cd->mn[2] = 'y';
	cd->mn[3] = 'v';
	cd->mn[4] = 'e';
	cd->mn[5] = '-';
	cd->mn[6] = 'N';
	cd->mn[7] = 'V';
	cd->mn[8] = 'M';
	cd->mn[9] = 'e';

	cd->fr[0] = '1';
	cd->fr[1] = '.';
	cd->fr[2] = '0';

	/* Num of submission commands that we can handle at a time (2^rab) */
	cd->rab   = 4;

	/* FreeBSD OUI */
	cd->ieee[0] = 0x58;
	cd->ieee[1] = 0x9c;
	cd->ieee[2] = 0xfc;

	cd->mic = 0;

	cd->mdts = 9; /* max data transfer size (2^mdts * CAP.MPSMIN) */

	cd->ver = 0x00010300;

	cd->oacs.format = 1;
	cd->acl = 2;
	cd->aerl = 4;

	cd->lpa.ns_smart = 0; /* TODO: support some simple things like SMART */
	cd->elpe = 0;	/* max error log page entries */
	cd->npss = 1;	/* number of power states support */

	/* Warning Composite Temperature Threshold */
	cd->wctemp = 0x0157;

	cd->sqes.min = 6;
	cd->sqes.max = 6;
	cd->cqes.min = 4;
	cd->cqes.max = 4;
	cd->nn = 1;	/* number of namespaces */

	/*cd->oncs = 0x08;*/ /* support Write Zeroes command */
	cd->fna = 0x03;

	cd->power_state[0].mp = 10;
}

static void
pci_nvme_init_nsdata(struct pci_nvme_softc *sc)
{
	struct nvme_namespace_data *nd;

	nd = &sc->nsdata;

	nd->nsze = sc->nvstore.size / sc->nvstore.sectsz;
	nd->ncap = nd->nsze;
	nd->nuse = nd->nsze;

	/* Get LBA and backstore information from backing store */
	nd->nlbaf = 1;
	nd->lbaf[0].ms = 0;
	nd->lbaf[0].lbads = sc->nvstore.sectsz_bits; /* LBA data-sz = 2^lbads */
	nd->lbaf[0].rp = 0;

	nd->flbas.format = 0;
}

static void
pci_nvme_reset(struct pci_nvme_softc *sc)
{
	DPRINTF(("%s\r\n", __func__));

	sc->regs.cap_lo.raw = 0;
	sc->regs.cap_lo.bits.mqes = sc->max_qentries;
	sc->regs.cap_lo.bits.cqr = 1;	/* contig queues */
	sc->regs.cap_lo.bits.ams = 0;
	sc->regs.cap_lo.bits.to = 60;	/* 60x500ms timeout */

	sc->regs.cap_hi.raw = 0;
	sc->regs.cap_hi.bits.dstrd = 0;	/* 32-bit doorbell stride */
	sc->regs.cap_hi.bits.css_nvm = 1;
	sc->regs.cap_hi.bits.mpsmin = 0;
	sc->regs.cap_hi.bits.mpsmax = 0;

	sc->regs.vs = 0x00010300;	/* NVMe v1.3 */

	sc->regs.cc.raw = 0;
	sc->regs.csts.raw = 0;

	if (sc->submit_queues != NULL) {
		pthread_mutex_lock(&sc->mtx);
		sc->num_cqueues = sc->num_squeues = sc->max_queues;

		for (int i = 0; i <= sc->max_queues; i++) {
			if (i != 0) {
				sc->submit_queues[i].qbase = NULL;
				sc->submit_queues[i].size = 0;
				sc->submit_queues[i].cqid = 0;

				sc->submit_queues[i].qbase = NULL;
				sc->compl_queues[i].size = 0;
			}
			sc->submit_queues[i].tail = 0;
			sc->submit_queues[i].head = 0;
			sc->submit_queues[i].busy = 0;

			sc->compl_queues[i].tail = 0;
			sc->compl_queues[i].head = 0;
		}

		pthread_mutex_unlock(&sc->mtx);
	} else
		sc->submit_queues = calloc(sc->max_queues + 1,
		                        sizeof(struct nvme_submission_queue));

	if (sc->compl_queues == NULL) {
		sc->compl_queues = calloc(sc->max_queues + 1,
		                        sizeof(struct nvme_completion_queue));

		for (int i = 0; i <= sc->num_cqueues; i++)
			pthread_mutex_init(&sc->compl_queues[i].mtx, NULL);
	}
}

static void
pci_nvme_init_controller(struct vmctx *ctx, struct pci_nvme_softc *sc)
{
	DPRINTF(("%s\r\n", __func__));

	sc->submit_queues[0].size = sc->regs.aqa.bits.asqs+1;
	sc->submit_queues[0].qbase = vm_map_gpa(ctx, sc->regs.asq,
	            sizeof(struct nvme_command) * (sc->regs.aqa.bits.asqs+1));

	DPRINTF(("%s mapping Admin-SQ guest 0x%lx, host: %p\r\n",
	        __func__, sc->regs.asq, sc->submit_queues[0].qbase));

	sc->compl_queues[0].size = sc->regs.aqa.bits.acqs+1;
	sc->compl_queues[0].qbase = vm_map_gpa(ctx, sc->regs.acq,
	         sizeof(struct nvme_completion) * (sc->regs.aqa.bits.acqs+1));
	DPRINTF(("%s mapping Admin-CQ guest 0x%lx, host: %p\r\n",
	        __func__, sc->regs.acq, sc->compl_queues[0].qbase));
}

static int
nvme_opc_delete_io_sq(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	uint16_t qid = command->cdw10 & 0xffff;

	DPRINTF(("%s DELETE_IO_SQ %u\r\n", __func__, qid));
	if (qid == 0 || qid > sc->num_cqueues) {
		WPRINTF(("%s NOT PERMITTED queue id %u / num_squeues %u\r\n",
		        __func__, qid, sc->num_squeues));
		compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
		compl->status.sc = NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return (1);
	}

	sc->submit_queues[qid].qbase = NULL;
	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_create_io_sq(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	if (command->cdw11 & NVME_CMD_CDW11_PC) {
		uint16_t qid = command->cdw10 & 0xffff;
		struct nvme_submission_queue *nsq;

		if (qid > sc->num_squeues) {
			WPRINTF(("%s queue index %u > num_squeues %u\r\n",
			        __func__, qid, sc->num_squeues));
			compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
			compl->status.sc = NVME_SC_INVALID_QUEUE_IDENTIFIER;
			return (1);
		}

		nsq = &sc->submit_queues[qid];
		nsq->size = ((command->cdw10 >> 16) & 0xffff) + 1;

		nsq->qbase = vm_map_gpa(sc->nsc_pi->pi_vmctx, command->prp1,
		              sizeof(struct nvme_command) * (size_t)nsq->size);
		nsq->cqid = (command->cdw11 >> 16) & 0xffff;
		nsq->qpriority = (command->cdw11 >> 1) & 0x03;

		DPRINTF(("%s sq %u size %u gaddr %p cqid %u\r\n", __func__,
		        qid, nsq->size, nsq->qbase, nsq->cqid));

		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_SUCCESS;

		DPRINTF(("%s completed creating IOSQ qid %u\r\n",
		         __func__, qid));
	} else {
		/* 
		 * Guest sent non-cont submission queue request.
		 * This setting is unsupported by this emulation.
		 */
		WPRINTF(("%s unsupported non-contig (list-based) "
		         "create i/o submission queue\r\n", __func__));

		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_INVALID_FIELD;
	}
	return (1);
}

static int
nvme_opc_delete_io_cq(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	uint16_t qid = command->cdw10 & 0xffff;

	DPRINTF(("%s DELETE_IO_CQ %u\r\n", __func__, qid));
	if (qid == 0 || qid > sc->num_cqueues) {
		WPRINTF(("%s queue index %u / num_cqueues %u\r\n",
		        __func__, qid, sc->num_cqueues));
		compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
		compl->status.sc = NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return (1);
	}

	sc->compl_queues[qid].qbase = NULL;
	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_create_io_cq(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	if (command->cdw11 & NVME_CMD_CDW11_PC) {
		uint16_t qid = command->cdw10 & 0xffff;
		struct nvme_completion_queue *ncq;

		if (qid > sc->num_cqueues) {
			WPRINTF(("%s queue index %u > num_cqueues %u\r\n",
			        __func__, qid, sc->num_cqueues));
			compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
			compl->status.sc = NVME_SC_INVALID_QUEUE_IDENTIFIER;
			return (1);
		}

		ncq = &sc->compl_queues[qid];
		ncq->intr_en = (command->cdw11 & NVME_CMD_CDW11_IEN) >> 1;
		ncq->intr_vec = (command->cdw11 >> 16) & 0xffff;
		ncq->size = ((command->cdw10 >> 16) & 0xffff) + 1;

		ncq->qbase = vm_map_gpa(sc->nsc_pi->pi_vmctx,
		             command->prp1,
		             sizeof(struct nvme_command) * (size_t)ncq->size);

		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_SUCCESS;
	} else {
		/* 
		 * Non-contig completion queue unsupported.
		 */
		WPRINTF(("%s unsupported non-contig (list-based) "
		         "create i/o completion queue\r\n",
		         __func__));

		/* 0x12 = Invalid Use of Controller Memory Buffer */
		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = 0x12;
	}

	return (1);
}

static int
nvme_opc_get_log_page(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	uint32_t logsize = (1 + ((command->cdw10 >> 16) & 0xFFF)) * 2;
	uint8_t logpage = command->cdw10 & 0xFF;
	void *data;

	DPRINTF(("%s log page %u len %u\r\n", __func__, logpage, logsize));

	if (logpage >= 1 && logpage <= 3)
		data = vm_map_gpa(sc->nsc_pi->pi_vmctx, command->prp1,
		                  PAGE_SIZE);

	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;

	switch (logpage) {
	case 0x01: /* Error information */
		memset(data, 0, logsize > PAGE_SIZE ? PAGE_SIZE : logsize);
		break;
	case 0x02: /* SMART/Health information */
		/* TODO: present some smart info */
		memset(data, 0, logsize > PAGE_SIZE ? PAGE_SIZE : logsize);
		break;
	case 0x03: /* Firmware slot information */
		memset(data, 0, logsize > PAGE_SIZE ? PAGE_SIZE : logsize);
		break;
	default:
		WPRINTF(("%s get log page %x command not supported\r\n",
		        __func__, logpage));

		compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
		compl->status.sc = NVME_SC_INVALID_LOG_PAGE;
	}

	return (1);
}

static int
nvme_opc_identify(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	void *dest;

	DPRINTF(("%s identify 0x%x nsid 0x%x\r\n", __func__,
	        command->cdw10 & 0xFF, command->nsid));

	switch (command->cdw10 & 0xFF) {
	case 0x00: /* return Identify Namespace data structure */
		dest = vm_map_gpa(sc->nsc_pi->pi_vmctx, command->prp1,
		                  sizeof(sc->nsdata));
		memcpy(dest, &sc->nsdata, sizeof(sc->nsdata));
		break;
	case 0x01: /* return Identify Controller data structure */
		dest = vm_map_gpa(sc->nsc_pi->pi_vmctx, command->prp1,
		                  sizeof(sc->ctrldata));
		memcpy(dest, &sc->ctrldata, sizeof(sc->ctrldata));
		break;
	case 0x02: /* list of 1024 active NSIDs > CDW1.NSID */
		dest = vm_map_gpa(sc->nsc_pi->pi_vmctx, command->prp1,
		                  sizeof(uint32_t) * 1024);
		((uint32_t *)dest)[0] = 1;
		((uint32_t *)dest)[1] = 0;
		break;
	case 0x11:
		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return (1);
	case 0x03: /* list of NSID structures in CDW1.NSID, 4096 bytes */
	case 0x10:
	case 0x12:
	case 0x13:
	case 0x14:
	case 0x15:
	default:
		DPRINTF(("%s unsupported identify command requested 0x%x\r\n",
		         __func__, command->cdw10 & 0xFF));
		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_INVALID_FIELD;
		return (1);
	}

	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_set_features(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	int feature = command->cdw10 & 0x0F;
	uint32_t iv;

	DPRINTF(("%s feature 0x%x\r\n", __func__, feature));
	compl->cdw0 = 0;

	switch (feature) {
	case NVME_FEAT_ARBITRATION:
		DPRINTF(("  arbitration 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_POWER_MANAGEMENT:
		DPRINTF(("  power management 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_LBA_RANGE_TYPE:
		DPRINTF(("  lba range 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_TEMPERATURE_THRESHOLD:
		DPRINTF(("  temperature threshold 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_ERROR_RECOVERY:
		DPRINTF(("  error recovery 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_VOLATILE_WRITE_CACHE:
		DPRINTF(("  volatile write cache 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_NUMBER_OF_QUEUES:
		sc->num_squeues = command->cdw11 & 0xFFFF;
		sc->num_cqueues = (command->cdw11 >> 16) & 0xFFFF;
		DPRINTF(("  number of queues (submit %u, completion %u)\r\n",
		        sc->num_squeues, sc->num_cqueues));

		if (sc->num_squeues == 0 || sc->num_squeues > sc->max_queues)
			sc->num_squeues = sc->max_queues;
		if (sc->num_cqueues == 0 || sc->num_cqueues > sc->max_queues)
			sc->num_cqueues = sc->max_queues;

		compl->cdw0 = (sc->num_squeues & 0xFFFF) |
		              ((sc->num_cqueues & 0xFFFF) << 16);

		break;
	case NVME_FEAT_INTERRUPT_COALESCING:
		DPRINTF(("  interrupt coalescing 0x%x\r\n", command->cdw11));

		/* in uS */
		sc->intr_coales_aggr_time = ((command->cdw11 >> 8) & 0xFF)*100;

		sc->intr_coales_aggr_thresh = command->cdw11 & 0xFF;
		break;
	case NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		iv = command->cdw11 & 0xFFFF;

		DPRINTF(("  interrupt vector configuration 0x%x\r\n",
		        command->cdw11));

		for (uint32_t i = 0; i <= sc->num_cqueues; i++) {
			if (sc->compl_queues[i].intr_vec == iv) {
				if (command->cdw11 & (1 << 16))
					sc->compl_queues[i].intr_en |=
					                      NVME_CQ_INTCOAL;  
				else
					sc->compl_queues[i].intr_en &=
					                     ~NVME_CQ_INTCOAL;  
			}
		}
		break;
	case NVME_FEAT_WRITE_ATOMICITY:
		DPRINTF(("  write atomicity 0x%x\r\n", command->cdw11));
		break;
	case NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		DPRINTF(("  async event configuration 0x%x\r\n",
		        command->cdw11));
		sc->async_ev_config = command->cdw11;
		break;
	case NVME_FEAT_SOFTWARE_PROGRESS_MARKER:
		DPRINTF(("  software progress marker 0x%x\r\n",
		        command->cdw11));
		break;
	case 0x0C:
		DPRINTF(("  autonomous power state transition 0x%x\r\n",
		        command->cdw11));
		break;
	default:
		WPRINTF(("%s invalid feature\r\n", __func__));
		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_INVALID_FIELD;
		return (1);
	}

	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_get_features(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	int feature = command->cdw10 & 0x0F;

	DPRINTF(("%s feature 0x%x\r\n", __func__, feature));

	compl->cdw0 = 0;

	switch (feature) {
	case NVME_FEAT_ARBITRATION:
		DPRINTF(("  arbitration\r\n"));
		break;
	case NVME_FEAT_POWER_MANAGEMENT:
		DPRINTF(("  power management\r\n"));
		break;
	case NVME_FEAT_LBA_RANGE_TYPE:
		DPRINTF(("  lba range\r\n"));
		break;
	case NVME_FEAT_TEMPERATURE_THRESHOLD:
		DPRINTF(("  temperature threshold\r\n"));
		switch ((command->cdw11 >> 20) & 0x3) {
		case 0:
			/* Over temp threshold */
			compl->cdw0 = 0xFFFF;
			break;
		case 1:
			/* Under temp threshold */
			compl->cdw0 = 0;
			break;
		default:
			WPRINTF(("  invalid threshold type select\r\n"));
			compl->status.sct = NVME_SCT_GENERIC;
			compl->status.sc = NVME_SC_INVALID_FIELD;
			return (1);
		}
		break;
	case NVME_FEAT_ERROR_RECOVERY:
		DPRINTF(("  error recovery\r\n"));
		break;
	case NVME_FEAT_VOLATILE_WRITE_CACHE:
		DPRINTF(("  volatile write cache\r\n"));
		break;
	case NVME_FEAT_NUMBER_OF_QUEUES:
		compl->cdw0 = 0;
		if (sc->num_squeues == 0)
			compl->cdw0 |= sc->max_queues & 0xFFFF;
		else
			compl->cdw0 |= sc->num_squeues & 0xFFFF;

		if (sc->num_cqueues == 0)
			compl->cdw0 |= (sc->max_queues & 0xFFFF) << 16;
		else
			compl->cdw0 |= (sc->num_cqueues & 0xFFFF) << 16;

		DPRINTF(("  number of queues (submit %u, completion %u)\r\n",
		        compl->cdw0 & 0xFFFF,
		        (compl->cdw0 >> 16) & 0xFFFF));

		break;
	case NVME_FEAT_INTERRUPT_COALESCING:
		DPRINTF(("  interrupt coalescing\r\n"));
		break;
	case NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		DPRINTF(("  interrupt vector configuration\r\n"));
		break;
	case NVME_FEAT_WRITE_ATOMICITY:
		DPRINTF(("  write atomicity\r\n"));
		break;
	case NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		DPRINTF(("  async event configuration\r\n"));
		sc->async_ev_config = command->cdw11;
		break;
	case NVME_FEAT_SOFTWARE_PROGRESS_MARKER:
		DPRINTF(("  software progress marker\r\n"));
		break;
	case 0x0C:
		DPRINTF(("  autonomous power state transition\r\n"));
		break;
	default:
		WPRINTF(("%s invalid feature 0x%x\r\n", __func__, feature));
		compl->status.sct = NVME_SCT_GENERIC;
		compl->status.sc = NVME_SC_INVALID_FIELD;
		return (1);
	}

	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_abort(struct pci_nvme_softc* sc, struct nvme_command* command,
	struct nvme_completion* compl)
{
	DPRINTF(("%s submission queue %u, command ID 0x%x\r\n", __func__,
	        command->cdw10 & 0xFFFF, (command->cdw10 >> 16) & 0xFFFF));

	/* TODO: search for the command ID and abort it */

	compl->cdw0 = 1;
	compl->status.sct = NVME_SCT_GENERIC;
	compl->status.sc = NVME_SC_SUCCESS;
	return (1);
}

static int
nvme_opc_async_event_req(struct pci_nvme_softc* sc,
	struct nvme_command* command, struct nvme_completion* compl)
{
	DPRINTF(("%s async event request 0x%x\r\n", __func__, command->cdw11));

	/*
	 * TODO: raise events when they happen based on the Set Features cmd.
	 * These events happen async, so only set completion successful if
	 * there is an event reflective of the request to get event.
	 */

	compl->status.sct = NVME_SCT_COMMAND_SPECIFIC;
	compl->status.sc = NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
	return (0);
}

static void
pci_nvme_handle_admin_cmd(struct pci_nvme_softc* sc, uint64_t value)
{
	struct nvme_completion compl;
	struct nvme_command *cmd;
	struct nvme_submission_queue *sq;
	struct nvme_completion_queue *cq;
	int do_intr = 0;
	uint16_t sqhead;

	DPRINTF(("%s index %u\r\n", __func__, (uint32_t)value));

	sq = &sc->submit_queues[0];

	sqhead = atomic_load_acq_short(&sq->head);

	if (atomic_testandset_int(&sq->busy, 1)) {
		DPRINTF(("%s SQ busy, head %u, tail %u\r\n",
		        __func__, sqhead, sq->tail));
		return;
	}

	while (sqhead != atomic_load_acq_short(&sq->tail)) {
		cmd = &(sq->qbase)[sqhead];
		compl.status.sct = 0;
		compl.status.sc = 0;

		switch (cmd->opc) {
		case NVME_OPC_DELETE_IO_SQ:
			DPRINTF(("%s command DELETE_IO_SQ\r\n", __func__));
			do_intr |= nvme_opc_delete_io_sq(sc, cmd, &compl);
			break;
		case NVME_OPC_CREATE_IO_SQ:
			DPRINTF(("%s command CREATE_IO_SQ\r\n", __func__));
			do_intr |= nvme_opc_create_io_sq(sc, cmd, &compl);
			break;
		case NVME_OPC_DELETE_IO_CQ:
			DPRINTF(("%s command DELETE_IO_CQ\r\n", __func__));
			do_intr |= nvme_opc_delete_io_cq(sc, cmd, &compl);
			break;
		case NVME_OPC_CREATE_IO_CQ:
			DPRINTF(("%s command CREATE_IO_CQ\r\n", __func__));
			do_intr |= nvme_opc_create_io_cq(sc, cmd, &compl);
			break;
		case NVME_OPC_GET_LOG_PAGE:
			DPRINTF(("%s command GET_LOG_PAGE\r\n", __func__));
			do_intr |= nvme_opc_get_log_page(sc, cmd, &compl);
			break;
		case NVME_OPC_IDENTIFY:
			DPRINTF(("%s command IDENTIFY\r\n", __func__));
			do_intr |= nvme_opc_identify(sc, cmd, &compl);
			break;
		case NVME_OPC_ABORT:
			DPRINTF(("%s command ABORT\r\n", __func__));
			do_intr |= nvme_opc_abort(sc, cmd, &compl);
			break;
		case NVME_OPC_SET_FEATURES:
			DPRINTF(("%s command SET_FEATURES\r\n", __func__));
			do_intr |= nvme_opc_set_features(sc, cmd, &compl);
			break;
		case NVME_OPC_GET_FEATURES:
			DPRINTF(("%s command GET_FEATURES\r\n", __func__));
			do_intr |= nvme_opc_get_features(sc, cmd, &compl);
			break;
		case NVME_OPC_ASYNC_EVENT_REQUEST:
			DPRINTF(("%s command ASYNC_EVENT_REQ\r\n", __func__));
			/* XXX dont care, unhandled for now
			do_intr |= nvme_opc_async_event_req(sc, cmd, &compl);
			*/
			break;
		default:
			WPRINTF(("0x%x command is not implemented", cmd->opc));
			compl.status.sct = 0;
			compl.status.sc = 0;
		}
	
		/* for now skip async event generation */
		if (cmd->opc != NVME_OPC_ASYNC_EVENT_REQUEST) {
			struct nvme_completion *cp;

			cq = &sc->compl_queues[0];

			cp = &(cq->qbase)[cq->tail];
			cp->sqid = 0;
			cp->sqhd = sqhead;
			cp->cid = cmd->cid;
			cp->status.sct = compl.status.sct;
			cp->status.sc = compl.status.sc;
			cp->status.p = !cp->status.p;
			cq->tail = (cq->tail + 1) % cq->size;
		}
		sqhead = (sqhead + 1) % sq->size;
	}

	atomic_store_short(&sq->head, sqhead);
	atomic_store_int(&sq->busy, 0);

	if (do_intr)
		pci_generate_msix(sc->nsc_pi, 0);

}

static int
pci_nvme_append_iov_req(struct pci_nvme_softc *sc, struct pci_nvme_ioreq *req,
	uint64_t gpaddr, size_t size, int do_write, uint64_t lba)
{
	int iovidx;

	if (req != NULL) {
		/* concatenate contig block-iovs to minimize number of iovs */
		if ((req->prev_gpaddr + req->prev_size) == gpaddr) {
			iovidx = req->io_req.br_iovcnt - 1;

			req->io_req.br_iov[iovidx].iov_base =
			    paddr_guest2host(req->sc->nsc_pi->pi_vmctx,
			                     req->prev_gpaddr, size);

			req->prev_size += size;
			req->io_req.br_resid += size;

			req->io_req.br_iov[iovidx].iov_len = req->prev_size;
		} else {
			pthread_mutex_lock(&req->mtx);

			iovidx = req->io_req.br_iovcnt;
			if (iovidx == NVME_MAX_BLOCKIOVS) {
				int err = 0;

				DPRINTF(("large I/O, doing partial req\r\n"));

				iovidx = 0;
				req->io_req.br_iovcnt = 0;

				req->io_req.br_callback = pci_nvme_io_partial;

				if (!do_write)
					err = blockif_read(sc->nvstore.ctx,
					                   &req->io_req);
				else
					err = blockif_write(sc->nvstore.ctx,
					                    &req->io_req);

				/* wait until req completes before cont */
				if (err == 0)
					pthread_cond_wait(&req->cv, &req->mtx);
			}
			if (iovidx == 0) {
				req->io_req.br_offset = lba;
				req->io_req.br_resid = 0;
				req->io_req.br_param = req;
			}

			req->io_req.br_iov[iovidx].iov_base =
			    paddr_guest2host(req->sc->nsc_pi->pi_vmctx,
			                     gpaddr, size);

			req->io_req.br_iov[iovidx].iov_len = size;

			req->prev_gpaddr = gpaddr;
			req->prev_size = size;
			req->io_req.br_resid += size;

			req->io_req.br_iovcnt++;

			pthread_mutex_unlock(&req->mtx);
		}
	} else {
		/* RAM buffer: read/write directly */
		void *p = sc->nvstore.ctx;
		void *gptr;

		if ((lba + size) > sc->nvstore.size) {
			WPRINTF(("%s write would overflow RAM\r\n", __func__));
			return (-1);
		}

		p = (void *)((uintptr_t)p + (uintptr_t)lba);
		gptr = paddr_guest2host(sc->nsc_pi->pi_vmctx, gpaddr, size);
		if (do_write) 
			memcpy(p, gptr, size);
		else
			memcpy(gptr, p, size);
	}
	return (0);
}

static void
pci_nvme_set_completion(struct pci_nvme_softc *sc,
	struct nvme_submission_queue *sq, int sqid, uint16_t cid,
	uint32_t cdw0, struct nvme_status status, int ignore_busy)
{
	struct nvme_completion_queue *cq = &sc->compl_queues[sq->cqid];
	struct nvme_completion *compl;
	int do_intr = 0;

	DPRINTF(("%s sqid %d cqid %u cid %u status: 0x%x 0x%x\r\n",
	        __func__, sqid, sq->cqid, cid, status.sct, status.sc));

	pthread_mutex_lock(&cq->mtx);

	assert(cq->qbase != NULL);

	compl = &cq->qbase[cq->tail];

	compl->sqhd = atomic_load_acq_short(&sq->head);
	compl->sqid = sqid;
	compl->cid = cid;

	compl->status.p = !compl->status.p;
	compl->status.sc = status.sc;
	compl->status.sct = status.sct;

	cq->tail = (cq->tail + 1) % cq->size;

	if (cq->intr_en & NVME_CQ_INTEN)
		do_intr = 1;

	pthread_mutex_unlock(&cq->mtx);

	if (ignore_busy || !atomic_load_acq_int(&sq->busy))
		if (do_intr)
			pci_generate_msix(sc->nsc_pi, cq->intr_vec);
}

static void
pci_nvme_release_ioreq(struct pci_nvme_softc *sc, struct pci_nvme_ioreq *req)
{
	req->sc = NULL;
	req->nvme_sq = NULL;
	req->sqid = 0;

	pthread_mutex_lock(&sc->mtx);

	req->next = sc->ioreqs_free;
	sc->ioreqs_free = req;
	sc->pending_ios--;

	/* when no more IO pending, can set to ready if device reset/enabled */
	if (sc->pending_ios == 0 &&
	    sc->regs.cc.bits.en && !sc->regs.csts.bits.rdy)
		sc->regs.csts.bits.rdy = 1;

	pthread_mutex_unlock(&sc->mtx);

	sem_post(&sc->iosemlock);
}

static struct pci_nvme_ioreq *
pci_nvme_get_ioreq(struct pci_nvme_softc *sc)
{
	struct pci_nvme_ioreq *req = NULL;;

	sem_wait(&sc->iosemlock);
	pthread_mutex_lock(&sc->mtx);

	req = sc->ioreqs_free;
	assert(req != NULL);

	sc->ioreqs_free = req->next;

	req->next = NULL;
	req->sc = sc;

	sc->pending_ios++;

	pthread_mutex_unlock(&sc->mtx);

	req->io_req.br_iovcnt = 0;
	req->io_req.br_offset = 0;
	req->io_req.br_resid = 0;
	req->io_req.br_param = req;
	req->prev_gpaddr = 0;
	req->prev_size = 0;

	return req;
}

static void
pci_nvme_io_done(struct blockif_req *br, int err)
{
	struct pci_nvme_ioreq *req = br->br_param;
	struct nvme_submission_queue *sq = req->nvme_sq;
	struct nvme_status status;

	DPRINTF(("%s error %d %s\r\n", __func__, err, strerror(err)));

	status.sct = NVME_SCT_GENERIC;

	/* TODO return correct error */
	if (err)
		status.sc = NVME_SC_DATA_TRANSFER_ERROR;
	else
		status.sc = NVME_SC_SUCCESS;

	pci_nvme_set_completion(req->sc, sq, req->sqid, req->cid, 0, status, 0);
	pci_nvme_release_ioreq(req->sc, req);
}

static void
pci_nvme_io_partial(struct blockif_req *br, int err)
{
	struct pci_nvme_ioreq *req = br->br_param;

	DPRINTF(("%s error %d %s\r\n", __func__, err, strerror(err)));

	pthread_cond_signal(&req->cv);
}


static void
pci_nvme_handle_io_cmd(struct pci_nvme_softc* sc, uint16_t idx)
{
	struct nvme_submission_queue *sq;
	struct nvme_status status;
	uint16_t sqhead;
	int err;

	/* handle all submissions up to sq->tail index */
	sq = &sc->submit_queues[idx];

	if (atomic_testandset_int(&sq->busy, 1)) {
		DPRINTF(("%s sqid %u busy\r\n", __func__, idx));
		return;
	}

	sqhead = atomic_load_acq_short(&sq->head);

	DPRINTF(("nvme_handle_io qid %u head %u tail %u cmdlist %p\r\n",
	         idx, sqhead, sq->tail, sq->qbase));

	while (sqhead != atomic_load_acq_short(&sq->tail)) {
		struct nvme_command *cmd;
		struct pci_nvme_ioreq *req = NULL;
		uint64_t lba;
		uint64_t nblocks, bytes, size, cpsz;

		/* TODO: support scatter gather list handling */

		cmd = &sq->qbase[sqhead];
		sqhead = (sqhead + 1) % sq->size;

		lba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;

		if (cmd->opc == NVME_OPC_FLUSH) {
			status.sct = NVME_SCT_GENERIC;
			status.sc = NVME_SC_SUCCESS;
			pci_nvme_set_completion(sc, sq, idx, cmd->cid, 0,
			                        status, 1);

			continue;
		} else if (cmd->opc == 0x08) {
			/* TODO: write zeroes */
			WPRINTF(("%s write zeroes lba 0x%lx blocks %u\r\n",
			        __func__, lba, cmd->cdw12 & 0xFFFF));
			status.sct = NVME_SCT_GENERIC;
			status.sc = NVME_SC_SUCCESS;
			pci_nvme_set_completion(sc, sq, idx, cmd->cid, 0,
			                        status, 1);

			continue;
		}

		nblocks = (cmd->cdw12 & 0xFFFF) + 1;

		bytes = nblocks * sc->nvstore.sectsz;

		if (sc->nvstore.type == NVME_STOR_BLOCKIF) {
			req = pci_nvme_get_ioreq(sc);
			req->nvme_sq = sq;
			req->sqid = idx;
		}

		/*
		 * If data starts mid-page and flows into the next page, then
		 * increase page count
		 */

		DPRINTF(("[h%u:t%u:n%u] %s starting LBA 0x%lx blocks %lu "
		         "(%lu-bytes)\r\n",
		         sqhead==0 ? sq->size-1 : sqhead-1, sq->tail, sq->size,
		         cmd->opc == NVME_OPC_WRITE ? "WRITE" : "READ",
		         lba, nblocks, bytes));

		cmd->prp1 &= ~(0x03UL);
		cmd->prp2 &= ~(0x03UL);

		DPRINTF((" prp1 0x%lx prp2 0x%lx\r\n", cmd->prp1, cmd->prp2));

		size = bytes;
		lba *= sc->nvstore.sectsz;

		cpsz = PAGE_SIZE - (cmd->prp1 % PAGE_SIZE);

		if (cpsz > bytes)
			cpsz = bytes;

		if (req != NULL) {
			req->io_req.br_offset = ((uint64_t)cmd->cdw11 << 32) |
			                        cmd->cdw10;
			req->opc = cmd->opc;
			req->cid = cmd->cid;
			req->nsid = cmd->nsid;
		}

		err = pci_nvme_append_iov_req(sc, req, cmd->prp1, cpsz,
		                              cmd->opc == NVME_OPC_WRITE, lba);
		lba += cpsz;
		size -= cpsz;

		if (size == 0)
			goto iodone;

		if (size <= PAGE_SIZE) {
			/* prp2 is second (and final) page in transfer */

			/*
			DPRINTF(("%s lba 0x%lx size %lu\r\n",
			        cmd->opc == NVME_OPC_WRITE ? "WRITE" : "READ",
			        lba, size));
			*/
			err = pci_nvme_append_iov_req(sc, req, cmd->prp2,
			               size, cmd->opc == NVME_OPC_WRITE, lba);
		} else {
			uint64_t *prp_list;
			int i;

			/* prp2 is pointer to a physical region page list */
			prp_list = paddr_guest2host(sc->nsc_pi->pi_vmctx,
			                            cmd->prp2, PAGE_SIZE);

			i = 0;
			while (size != 0) {
				cpsz = MIN(size, PAGE_SIZE);

				/*
				 * Move to linked physical region page list
				 * in last item.
				 */ 
				if (i == (NVME_PRP2_ITEMS-1) &&
				    size > PAGE_SIZE) {
					assert((prp_list[i] & (PAGE_SIZE-1)) == 0);
					prp_list = paddr_guest2host(
					              sc->nsc_pi->pi_vmctx,
					              prp_list[i], PAGE_SIZE);
					i = 0;
				}
				if (prp_list[i] == 0) {
					WPRINTF(("PRP2[%d] = 0 !!!\r\n", i));
					err = 1;
					break;
				}

				err = pci_nvme_append_iov_req(sc, req,
				           prp_list[i], cpsz,
				           cmd->opc == NVME_OPC_WRITE, lba);
				if (err)
					break;

				lba += cpsz;
				size -= cpsz;
				i++;
			}
		}

iodone:
		if (sc->nvstore.type == NVME_STOR_RAM) {
			struct nvme_status status;

			status.sct = NVME_SCT_GENERIC;
			status.sc = (err == 0) ? NVME_SC_SUCCESS
			                       : NVME_SC_LBA_OUT_OF_RANGE;

			pci_nvme_set_completion(sc, sq, idx, cmd->cid, 0,
			                        status, 1);

			continue;
		}


		if (err)
			goto do_error;

		req->io_req.br_callback = pci_nvme_io_done;

		err = 0;
		switch (cmd->opc) {
		case NVME_OPC_READ:
			err = blockif_read(sc->nvstore.ctx, &req->io_req);
			break;
		case NVME_OPC_WRITE:
			err = blockif_write(sc->nvstore.ctx, &req->io_req);
			break;
		default:
			WPRINTF(("%s unhandled io command 0x%x\r\n",
			        __func__, cmd->opc));
			err = 1;
		}

do_error:
		if (err) {
			struct nvme_status status;

			status.sct = NVME_SCT_GENERIC;
			status.sc = NVME_SC_DATA_TRANSFER_ERROR;

			pci_nvme_set_completion(sc, sq, idx, cmd->cid, 0,
			                        status, 1);
			pci_nvme_release_ioreq(sc, req);
		}
	}

	/* DPRINTF(("%s done, unsetting busy\r\n", __func__)); */
	atomic_store_short(&sq->head, sqhead);
	atomic_store_int(&sq->busy, 0);
}

static void
pci_nvme_handle_doorbell(struct vmctx *ctx, struct pci_nvme_softc* sc,
	uint64_t idx, int is_sq, uint64_t value)
{
	DPRINTF(("nvme doorbell %lu, %s, val 0x%lx\r\n",
	        idx, is_sq ? "SQ" : "CQ", value & 0xFFFF));

	if (is_sq) {
		atomic_store_short(&sc->submit_queues[idx].tail,
		                   (uint16_t)value);

		if (idx == 0) {
			pci_nvme_handle_admin_cmd(sc, value);
		} else {
			/* submission queue; handle new entries in SQ */
			if (idx > sc->num_squeues) {
				WPRINTF(("%s SQ index %lu overflow from "
				         "guest (max %u)\r\n",
				         __func__, idx, sc->num_squeues));
				/*
				sts.sct = NVME_SCT_COMMAND_SPECIFIC;
				sts.sc = NVME_SC_INVALID_QUEUE_IDENTIFIER;
				pci_nvme_append_completion(sc, 0, 0, 0, idx,
				                           0, status);
				*/
				return;
			}
			pci_nvme_handle_io_cmd(sc, (uint16_t)idx);
		}
	} else {
		if (idx > sc->num_cqueues) {
			WPRINTF(("%s queue index %lu overflow from "
			         "guest (max %u)\r\n",
			         __func__, idx, sc->num_cqueues));
			return;
		}

		sc->compl_queues[idx].head = (uint16_t)value;
	}
}

static void
pci_nvme_bar0_reg_dumps(const char *func, uint64_t offset, int iswrite)
{
	const char *s = iswrite ? "WRITE" : "READ";

	switch (offset) {
	case NVME_CR_CAP_LOW:
		DPRINTF(("%s %s NVME_CR_CAP_LOW\r\n", func, s));
		break;
	case NVME_CR_CAP_HI:
		DPRINTF(("%s %s NVME_CR_CAP_HI\r\n", func, s));
		break;
	case NVME_CR_VS:
		DPRINTF(("%s %s NVME_CR_VS\r\n", func, s));
		break;
	case NVME_CR_INTMS:
		DPRINTF(("%s %s NVME_CR_INTMS\r\n", func, s));
		break;
	case NVME_CR_INTMC:
		DPRINTF(("%s %s NVME_CR_INTMC\r\n", func, s));
		break;
	case NVME_CR_CC:
		DPRINTF(("%s %s NVME_CR_CC\r\n", func, s));
		break;
	case NVME_CR_CSTS:
		DPRINTF(("%s %s NVME_CR_CSTS\r\n", func, s));
		break;
	case NVME_CR_NSSR:
		DPRINTF(("%s %s NVME_CR_NSSR\r\n", func, s));
		break;
	case NVME_CR_AQA:
		DPRINTF(("%s %s NVME_CR_AQA\r\n", func, s));
		break;
	case NVME_CR_ASQ_LOW:
		DPRINTF(("%s %s NVME_CR_ASQ_LOW\r\n", func, s));
		break;
	case NVME_CR_ASQ_HI:
		DPRINTF(("%s %s NVME_CR_ASQ_HI\r\n", func, s));
		break;
	case NVME_CR_ACQ_LOW:
		DPRINTF(("%s %s NVME_CR_ACQ_LOW\r\n", func, s));
		break;
	case NVME_CR_ACQ_HI:
		DPRINTF(("%s %s NVME_CR_ACQ_HI\r\n", func, s));
		break;
	default:
		DPRINTF(("unknown nvme bar-0 offset 0x%lx\r\n", offset));
	}

}

static void
pci_nvme_write_bar_0(struct vmctx *ctx, struct pci_nvme_softc* sc,
	uint64_t offset, int size, uint64_t value)
{
	union cc_register ccreg;

	if (offset >= NVME_DOORBELL_OFFSET) {
		uint64_t belloffset = offset - NVME_DOORBELL_OFFSET;
		uint64_t idx = belloffset / 8; /* door bell size = 2*int */
		int is_sq = (belloffset % 8) < 4;

		if (belloffset > ((sc->max_queues+1) * 8 - 4)) {
			WPRINTF(("guest attempted an overflow write offset "
			         "0x%lx, val 0x%lx in %s",
			         offset, value, __func__));
			return;
		}

		pci_nvme_handle_doorbell(ctx, sc, idx, is_sq, value);
		return;
	}

	DPRINTF(("nvme-write offset 0x%lx, size %d, value 0x%lx\r\n",
	        offset, size, value));

	if (size != 4) {
		WPRINTF(("guest wrote invalid size %d (offset 0x%lx, "
		         "val 0x%lx) to bar0 in %s",
		         size, offset, value, __func__));
		/* TODO: shutdown device */
		return;
	}

	pci_nvme_bar0_reg_dumps(__func__, offset, 1);

	pthread_mutex_lock(&sc->mtx);

	switch (offset) {
	case NVME_CR_CAP_LOW:
	case NVME_CR_CAP_HI:
		/* readonly */
		break;
	case NVME_CR_VS:
		/* readonly */
		break;
	case NVME_CR_INTMS:
		/* MSI-X, so ignore */
		break;
	case NVME_CR_INTMC:
		/* MSI-X, so ignore */
		break;
	case NVME_CR_CC:
		ccreg.raw = (uint32_t)value;

		DPRINTF(("%s NVME_CR_CC en %x css %x shn %x iosqes %u "
		         "iocqes %u\r\n",
		        __func__, ccreg.bits.en, ccreg.bits.css,
		        ccreg.bits.shn, ccreg.bits.iosqes, ccreg.bits.iocqes));

		if (ccreg.bits.shn) {
			/* perform shutdown - flush out data to backend */
			sc->regs.csts.bits.shst = NVME_SHST_COMPLETE;
		}
		if (ccreg.bits.en != sc->regs.cc.bits.en) {
			if (ccreg.bits.en == 0)
				/* transition 1-> causes controller reset */
				pci_nvme_reset(sc);
			else
				pci_nvme_init_controller(ctx, sc);
		}

		sc->regs.cc.bits.iocqes = ccreg.bits.iocqes;
		sc->regs.cc.bits.iosqes = ccreg.bits.iosqes;
		sc->regs.cc.bits.en = ccreg.bits.en;
		if (ccreg.bits.en == 0) {
			sc->regs.cc.bits.ams = ccreg.bits.ams;
			sc->regs.cc.bits.ams = ccreg.bits.ams;
			sc->regs.cc.bits.mps = ccreg.bits.mps;
			sc->regs.cc.bits.css = ccreg.bits.css;
			sc->regs.csts.bits.rdy = 0;
		} else if (sc->pending_ios == 0)
			sc->regs.csts.bits.rdy = 1;
		break;
	case NVME_CR_CSTS:
		break;
	case NVME_CR_NSSR:
		/* ignore writes; don't support subsystem reset */
		break;
	case NVME_CR_AQA:
		sc->regs.aqa.raw = (uint32_t)value;
		break;
	case NVME_CR_ASQ_LOW:
		sc->regs.asq = (sc->regs.asq & (0xFFFFFFFF00000000)) |
		               (0xFFFFF000 & value);
		break;
	case NVME_CR_ASQ_HI:
		sc->regs.asq = (sc->regs.asq & (0x00000000FFFFFFFF)) |
		               (value << 32);
		break;
	case NVME_CR_ACQ_LOW:
		sc->regs.acq = (sc->regs.acq & (0xFFFFFFFF00000000)) |
		               (0xFFFFF000 & value);
		break;
	case NVME_CR_ACQ_HI:
		sc->regs.acq = (sc->regs.acq & (0x00000000FFFFFFFF)) |
		               (value << 32);
		break;
	default:
		DPRINTF(("%s unknown offset 0x%lx, value 0x%lx size %d\r\n",
		         __func__, offset, value, size));
	}
	pthread_mutex_unlock(&sc->mtx);
}

static void
pci_nvme_write(struct vmctx *ctx, int vcpu, struct pci_devinst *pi,
                int baridx, uint64_t offset, int size, uint64_t value)
{
	struct pci_nvme_softc* sc = pi->pi_arg;

	if (baridx == pci_msix_table_bar(pi) ||
	    baridx == pci_msix_pba_bar(pi)) {
		DPRINTF(("nvme-write baridx %d, msix: off 0x%lx, size %d, "
		         " value 0x%lx\r\n", baridx, offset, size, value));

		pci_emul_msix_twrite(pi, offset, size, value);
		return;
	}

	switch (baridx) {
	case 0:
		pci_nvme_write_bar_0(ctx, sc, offset, size, value);
		break;

	default:
		DPRINTF(("%s unknown baridx %d, val 0x%lx\r\n",
		         __func__, baridx, value));
	}
}

static uint64_t pci_nvme_read_bar_0(struct pci_nvme_softc* sc,
	uint64_t offset, int size)
{
	uint64_t value;

	pci_nvme_bar0_reg_dumps(__func__, offset, 0);

	if (offset < NVME_DOORBELL_OFFSET) {
		void *p = &(sc->regs);
		pthread_mutex_lock(&sc->mtx);
		memcpy(&value, (void *)((uintptr_t)p + offset), size);
		pthread_mutex_unlock(&sc->mtx);
	} else {
		value = 0;
                WPRINTF(("pci_nvme: read invalid offset %ld\r\n", offset));
	}

	switch (size) {
	case 1:
		value &= 0xFF;
		break;
	case 2:
		value &= 0xFFFF;
		break;
	case 4:
		value &= 0xFFFFFFFF;
		break;
	}

	DPRINTF(("   nvme-read offset 0x%lx, size %d -> value 0x%x\r\n",
	         offset, size, (uint32_t)value));

	return (value);
}



static uint64_t
pci_nvme_read(struct vmctx *ctx, int vcpu, struct pci_devinst *pi, int baridx,
    uint64_t offset, int size)
{
	struct pci_nvme_softc* sc = pi->pi_arg;

	if (baridx == pci_msix_table_bar(pi) ||
	    baridx == pci_msix_pba_bar(pi)) {
		DPRINTF(("nvme-read bar: %d, msix: regoff 0x%lx, size %d\r\n",
		        baridx, offset, size));

		return pci_emul_msix_tread(pi, offset, size);
	}

	switch (baridx) {
	case 0:
       		return pci_nvme_read_bar_0(sc, offset, size);

	default:
		DPRINTF(("unknown bar %d, 0x%lx\r\n", baridx, offset));
	}

	return (0);
}


static int
pci_nvme_parse_opts(struct pci_nvme_softc *sc, char *opts)
{
	char bident[sizeof("XX:X:X")];
	char	*uopt, *xopts, *config;
	uint32_t sectsz;
	int optidx;

	sc->max_queues = NVME_QUEUES;
	sc->max_qentries = NVME_MAX_QENTRIES;
	sc->ioslots = NVME_IOSLOTS;
	sc->num_squeues = sc->max_queues;
	sc->num_cqueues = sc->max_queues;
	sectsz = 0;

	uopt = strdup(opts);
	optidx = 0;
	snprintf(sc->ctrldata.sn, sizeof(sc->ctrldata.sn),
	         "NVME-%d-%d", sc->nsc_pi->pi_slot, sc->nsc_pi->pi_func);
	for (xopts = strtok(uopt, ",");
	     xopts != NULL;
	     xopts = strtok(NULL, ",")) {

		if ((config = strchr(xopts, '=')) != NULL)
			*config++ = '\0';

		if (!strcmp("maxq", xopts)) {
			sc->max_queues = atoi(config);
		} else if (!strcmp("qsz", xopts)) {
			sc->max_qentries = atoi(config);
		} else if (!strcmp("ioslots", xopts)) {
			sc->ioslots = atoi(config);
		} else if (!strcmp("sectsz", xopts)) {
			sectsz = atoi(config);
		} else if (!strcmp("ser", xopts)) {
			memset(sc->ctrldata.sn, 0, sizeof(sc->ctrldata.sn));
			strncpy(sc->ctrldata.sn, config,
			        sizeof(sc->ctrldata.sn));
		} else if (!strcmp("ram", xopts)) {
			uint64_t sz = strtoull(&xopts[4], NULL, 10);

			sc->nvstore.type = NVME_STOR_RAM;
			sc->nvstore.size = sz * 1024 * 1024;
			sc->nvstore.ctx = calloc(1, sc->nvstore.size);
			sc->nvstore.sectsz = 4096;
			sc->nvstore.sectsz_bits = 12;
			if (sc->nvstore.ctx == NULL) {
				perror("Unable to allocate RAM");
				return (-1);
			}
		} else if (optidx == 0) {
			snprintf(bident, sizeof(bident), "%d:%d",
			         sc->nsc_pi->pi_slot, sc->nsc_pi->pi_func);
			sc->nvstore.ctx = blockif_open(xopts, bident);
			if (sc->nvstore.ctx == NULL) {
				perror("Could not open backing file");
				return (-1);
			}
			sc->nvstore.type = NVME_STOR_BLOCKIF;
			sc->nvstore.size = blockif_size(sc->nvstore.ctx);
		} else {
			fprintf(stderr, "Invalid option %s\n", xopts);
			return (-1);
		}

		optidx++;
	}
	if (sc->nvstore.ctx == NULL || sc->nvstore.size == 0) {
		fprintf(stderr, "backing store not specified\n");
		return (-1);
	}
	if (sectsz == 512 || sectsz == 4096 || sectsz == 8192)
		sc->nvstore.sectsz = sectsz;
	else if (sc->nvstore.type != NVME_STOR_RAM)
		sc->nvstore.sectsz = blockif_sectsz(sc->nvstore.ctx);
	for (sc->nvstore.sectsz_bits = 9;
	     (1 << sc->nvstore.sectsz_bits) < sc->nvstore.sectsz;
	     sc->nvstore.sectsz_bits++)
		;


	if (sc->max_queues == 0) {
		fprintf(stderr, "Invalid maxq option\n");
		return (-1);
	}
	if (sc->max_qentries <= 0) {
		fprintf(stderr, "Invalid qsz option\n");
		return (-1);
	}
	if (sc->ioslots <= 0) {
		fprintf(stderr, "Invalid ioslots option\n");
		return (-1);
	}

	return (0);
}

static int
pci_nvme_init(struct vmctx *ctx, struct pci_devinst *pi, char *opts)
{
	struct pci_nvme_softc *sc;
	uint32_t pci_membar_sz;
	int	error;

	error = 0;

	sc = calloc(1, sizeof(struct pci_nvme_softc));
	pi->pi_arg = sc;
	sc->nsc_pi = pi;

	error = pci_nvme_parse_opts(sc, opts);
	if (error < 0)
		goto done;
	else
		error = 0;

	sc->ioreqs = calloc(sc->ioslots, sizeof(struct pci_nvme_ioreq));
	for (int i = 0; i < sc->ioslots; i++) {
		if (i < (sc->ioslots-1))
			sc->ioreqs[i].next = &sc->ioreqs[i+1];
		pthread_mutex_init(&sc->ioreqs[i].mtx, NULL);
		pthread_cond_init(&sc->ioreqs[i].cv, NULL);
	}
	sc->ioreqs_free = sc->ioreqs;
	sc->intr_coales_aggr_thresh = 1;

	pci_set_cfgdata16(pi, PCIR_DEVICE, 0x0A0A);
	pci_set_cfgdata16(pi, PCIR_VENDOR, 0xFB5D);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_STORAGE);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_STORAGE_NVM);
	pci_set_cfgdata8(pi, PCIR_PROGIF,
	                 PCIP_STORAGE_NVM_ENTERPRISE_NVMHCI_1_0);

	/* allocate size of nvme registers + doorbell space for all queues */
	pci_membar_sz = sizeof(struct nvme_registers) +
	                2*sizeof(uint32_t)*(sc->max_queues);

	DPRINTF(("nvme membar size: %u\r\n", pci_membar_sz));

	error = pci_emul_alloc_bar(pi, 0, PCIBAR_MEM64, pci_membar_sz);
	if (error) {
		WPRINTF(("%s pci alloc mem bar failed\r\n", __func__));
		goto done;
	}

	error = pci_emul_add_msixcap(pi, sc->max_queues, NVME_MSIX_BAR);
	if (error) {
		WPRINTF(("%s pci add msixcap failed\r\n", __func__));
		goto done;
	}

	pthread_mutex_init(&sc->mtx, NULL);
	sem_init(&sc->iosemlock, 0, sc->ioslots);

	pci_nvme_reset(sc);
	pci_nvme_init_ctrldata(sc);
	pci_nvme_init_nsdata(sc);

	pci_lintr_request(pi);

done:
	return (error);
}


struct pci_devemu pci_de_nvme = {
	.pe_emu =	"nvme",
	.pe_init =	pci_nvme_init,
	.pe_barwrite =	pci_nvme_write,
	.pe_barread =	pci_nvme_read
};
PCI_EMUL_SET(pci_de_nvme);
