/*-
 * Copyright (c) 1998 - 2008 Søren Schmidt <sos@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "opt_ata.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/taskqueue.h>

#include <sys/mplock2.h>		/* for {get,rel}_mplock() */

#include "ata-all.h"
#include "ata-pci.h"
#include "ata_if.h"

/*
 * SATA support functions
 */
void
ata_sata_phy_event(void *context, int dummy)
{
    struct ata_connect_task *tp = (struct ata_connect_task *)context;
    struct ata_channel *ch = device_get_softc(tp->dev);
    device_t *children;
    int nchildren, i;

    get_mplock();
    if (tp->action == ATA_C_ATTACH) {
	if (bootverbose)
	    device_printf(tp->dev, "CONNECTED\n");
	ATA_RESET(tp->dev);
	ata_identify(tp->dev);
    }
    if (tp->action == ATA_C_DETACH) {
	if (!device_get_children(tp->dev, &children, &nchildren)) {
	    for (i = 0; i < nchildren; i++)
		if (children[i])
		    device_delete_child(tp->dev, children[i]);
	    kfree(children, M_TEMP);
	}
	lockmgr(&ch->state_mtx, LK_EXCLUSIVE);
	ch->state = ATA_IDLE;
	lockmgr(&ch->state_mtx, LK_RELEASE);
	if (bootverbose)
	    device_printf(tp->dev, "DISCONNECTED\n");
    }
    rel_mplock();
    kfree(tp, M_ATA);
}

void
ata_sata_phy_check_events(device_t dev)
{
    struct ata_channel *ch = device_get_softc(dev);
    u_int32_t error = ATA_IDX_INL(ch, ATA_SERROR);

    /* clear error bits/interrupt */
    ATA_IDX_OUTL(ch, ATA_SERROR, error);

    /* do we have any events flagged ? */
    if (error) {
	struct ata_connect_task *tp;
	u_int32_t status = ATA_IDX_INL(ch, ATA_SSTATUS);

	/* if we have a connection event deal with it */
	if ((error & ATA_SE_PHY_CHANGED) &&
	    (tp = (struct ata_connect_task *)
		  kmalloc(sizeof(struct ata_connect_task),
			 M_ATA, M_INTWAIT | M_ZERO))) {

	    if (((status & ATA_SS_CONWELL_MASK) == ATA_SS_CONWELL_GEN1) ||
		((status & ATA_SS_CONWELL_MASK) == ATA_SS_CONWELL_GEN2)) {
		if (bootverbose)
		    device_printf(ch->dev, "CONNECT requested\n");
		tp->action = ATA_C_ATTACH;
	    }
	    else {
		if (bootverbose)
		    device_printf(ch->dev, "DISCONNECT requested\n");
		tp->action = ATA_C_DETACH;
	    }
	    tp->dev = ch->dev;
	    TASK_INIT(&tp->task, 0, ata_sata_phy_event, tp);
	    taskqueue_enqueue(taskqueue_thread[mycpuid], &tp->task);
	}
    }
}

static int
ata_sata_connect(struct ata_channel *ch)
{
    u_int32_t status;
    int timeout;

    /* wait up to 1 second for "connect well" */
    for (timeout = 0; timeout < 100 ; timeout++) {
	status = ATA_IDX_INL(ch, ATA_SSTATUS);
	if ((status & ATA_SS_CONWELL_MASK) == ATA_SS_CONWELL_GEN1 ||
	    (status & ATA_SS_CONWELL_MASK) == ATA_SS_CONWELL_GEN2)
	    break;
	ata_udelay(10000);
    }
    if (timeout >= 100) {
	if (bootverbose)
	    device_printf(ch->dev, "SATA connect status=%08x\n", status);
	return 0;
    }
    if (bootverbose)
	device_printf(ch->dev, "SATA connect time=%dms\n", timeout * 10);

    /* clear SATA error register */
    ATA_IDX_OUTL(ch, ATA_SERROR, ATA_IDX_INL(ch, ATA_SERROR));

    return 1;
}

int
ata_sata_phy_reset(device_t dev)
{
    struct ata_channel *ch = device_get_softc(dev);
    int loop, retry;

    if ((ATA_IDX_INL(ch, ATA_SCONTROL) & ATA_SC_DET_MASK) == ATA_SC_DET_IDLE)
	return ata_sata_connect(ch);

    for (retry = 0; retry < 10; retry++) {
	for (loop = 0; loop < 10; loop++) {
	    ATA_IDX_OUTL(ch, ATA_SCONTROL, ATA_SC_DET_RESET);
	    ata_udelay(100);
	    if ((ATA_IDX_INL(ch, ATA_SCONTROL) & ATA_SC_DET_MASK) ==
		ATA_SC_DET_RESET)
		break;
	}
	ata_udelay(5000);
	for (loop = 0; loop < 10; loop++) {
	    ATA_IDX_OUTL(ch, ATA_SCONTROL, ATA_SC_DET_IDLE |
					   ATA_SC_IPM_DIS_PARTIAL |
					   ATA_SC_IPM_DIS_SLUMBER);
	    ata_udelay(100);
	    if ((ATA_IDX_INL(ch, ATA_SCONTROL) & ATA_SC_DET_MASK) == 0)
		return ata_sata_connect(ch);
	}
    }
    return 0;
}

void
ata_sata_setmode(device_t dev, int mode)
{
    struct ata_device *atadev = device_get_softc(dev);

    /*
     * if we detect that the device isn't a real SATA device we limit
     * the transfer mode to UDMA5/ATA100.
     * this works around the problems some devices has with the
     * Marvell 88SX8030 SATA->PATA converters and UDMA6/ATA133.
     */
    if (atadev->param.satacapabilities != 0x0000 &&
	atadev->param.satacapabilities != 0xffff) {
	struct ata_channel *ch = device_get_softc(device_get_parent(dev));

	/* on some drives we need to set the transfer mode */
	ata_controlcmd(dev, ATA_SETFEATURES, ATA_SF_SETXFER, 0,
		       ata_limit_mode(dev, mode, ATA_UDMA6));

	/* query SATA STATUS for the speed */
        if (ch->r_io[ATA_SSTATUS].res &&
	   ((ATA_IDX_INL(ch, ATA_SSTATUS) & ATA_SS_CONWELL_MASK) ==
	    ATA_SS_CONWELL_GEN2))
	    atadev->mode = ATA_SA300;
	else
	    atadev->mode = ATA_SA150;
    }
    else {
	mode = ata_limit_mode(dev, mode, ATA_UDMA5);
	if (!ata_controlcmd(dev, ATA_SETFEATURES, ATA_SF_SETXFER, 0, mode))
	    atadev->mode = mode;
    }
}

int
ata_request2fis_h2d(struct ata_request *request, u_int8_t *fis)
{
    struct ata_device *atadev = device_get_softc(request->dev);

    if (request->flags & ATA_R_ATAPI) {
	fis[0] = 0x27;			/* host to device */
	fis[1] = 0x80 | (atadev->unit & 0x0f);
	fis[2] = ATA_PACKET_CMD;
	if (request->flags & (ATA_R_READ | ATA_R_WRITE))
	    fis[3] = ATA_F_DMA;
	else {
	    fis[5] = request->transfersize;
	    fis[6] = request->transfersize >> 8;
	}
	fis[7] = ATA_D_LBA;
	fis[15] = ATA_A_4BIT;
	return 20;
    }
    else {
	ata_modify_if_48bit(request);
	fis[0] = 0x27;			/* host to device */
	fis[1] = 0x80 | (atadev->unit & 0x0f);
	fis[2] = request->u.ata.command;
	fis[3] = request->u.ata.feature;
	fis[4] = request->u.ata.lba;
	fis[5] = request->u.ata.lba >> 8;
	fis[6] = request->u.ata.lba >> 16;
	fis[7] = ATA_D_LBA;
	if (!(atadev->flags & ATA_D_48BIT_ACTIVE))
	    fis[7] |= (ATA_D_IBM | (request->u.ata.lba >> 24 & 0x0f));
	fis[8] = request->u.ata.lba >> 24;
	fis[9] = request->u.ata.lba >> 32;
	fis[10] = request->u.ata.lba >> 40;
	fis[11] = request->u.ata.feature >> 8;
	fis[12] = request->u.ata.count;
	fis[13] = request->u.ata.count >> 8;
	fis[15] = ATA_A_4BIT;
	return 20;
    }
    return 0;
}
