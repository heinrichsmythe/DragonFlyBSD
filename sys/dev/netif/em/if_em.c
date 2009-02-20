/*
 * Copyright (c) 2004 Joerg Sonnenberger <joerg@bec.de>.  All rights reserved.
 *
 * Copyright (c) 2001-2008, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Copyright (c) 2005 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/sys/dev/netif/em/if_em.c,v 1.80 2008/09/17 08:51:29 sephe Exp $
 */
/*
 * SERIALIZATION API RULES:
 *
 * - If the driver uses the same serializer for the interrupt as for the
 *   ifnet, most of the serialization will be done automatically for the
 *   driver.
 *
 * - ifmedia entry points will be serialized by the ifmedia code using the
 *   ifnet serializer.
 *
 * - if_* entry points except for if_input will be serialized by the IF
 *   and protocol layers.
 *
 * - The device driver must be sure to serialize access from timeout code
 *   installed by the device driver.
 *
 * - The device driver typically holds the serializer at the time it wishes
 *   to call if_input.
 *
 * - We must call lwkt_serialize_handler_enable() prior to enabling the
 *   hardware interrupt and lwkt_serialize_handler_disable() after disabling
 *   the hardware interrupt in order to avoid handler execution races from
 *   scheduled interrupt threads.
 *
 *   NOTE!  Since callers into the device driver hold the ifnet serializer,
 *   the device driver may be holding a serializer at the time it calls
 *   if_input even if it is not serializer-aware.
 */

#include "opt_polling.h"
#include "opt_serializer.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ifq_var.h>
#include <net/vlan/if_vlan_var.h>
#include <net/vlan/if_vlan_ether.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <bus/pci/pcivar.h>
#include <bus/pci/pcireg.h>

#include <dev/netif/ig_hal/e1000_api.h>
#include <dev/netif/ig_hal/e1000_82571.h>
#include <dev/netif/em/if_em.h>

#define EM_NAME	"Intel(R) PRO/1000 Network Connection "
#define EM_VER	" 6.9.6"

#define EM_DEVICE(id)	\
	{ EM_VENDOR_ID, E1000_DEV_ID_##id, EM_NAME #id EM_VER }
#define EM_DEVICE_NULL	{ 0, 0, NULL }

static const struct em_vendor_info em_vendor_info_array[] = {
	EM_DEVICE(82540EM),
	EM_DEVICE(82540EM_LOM),
	EM_DEVICE(82540EP),
	EM_DEVICE(82540EP_LOM),
	EM_DEVICE(82540EP_LP),

	EM_DEVICE(82541EI),
	EM_DEVICE(82541ER),
	EM_DEVICE(82541ER_LOM),
	EM_DEVICE(82541EI_MOBILE),
	EM_DEVICE(82541GI),
	EM_DEVICE(82541GI_LF),
	EM_DEVICE(82541GI_MOBILE),

	EM_DEVICE(82542),

	EM_DEVICE(82543GC_FIBER),
	EM_DEVICE(82543GC_COPPER),

	EM_DEVICE(82544EI_COPPER),
	EM_DEVICE(82544EI_FIBER),
	EM_DEVICE(82544GC_COPPER),
	EM_DEVICE(82544GC_LOM),

	EM_DEVICE(82545EM_COPPER),
	EM_DEVICE(82545EM_FIBER),
	EM_DEVICE(82545GM_COPPER),
	EM_DEVICE(82545GM_FIBER),
	EM_DEVICE(82545GM_SERDES),

	EM_DEVICE(82546EB_COPPER),
	EM_DEVICE(82546EB_FIBER),
	EM_DEVICE(82546EB_QUAD_COPPER),
	EM_DEVICE(82546GB_COPPER),
	EM_DEVICE(82546GB_FIBER),
	EM_DEVICE(82546GB_SERDES),
	EM_DEVICE(82546GB_PCIE),
	EM_DEVICE(82546GB_QUAD_COPPER),
	EM_DEVICE(82546GB_QUAD_COPPER_KSP3),

	EM_DEVICE(82547EI),
	EM_DEVICE(82547EI_MOBILE),
	EM_DEVICE(82547GI),

	EM_DEVICE(82571EB_COPPER),
	EM_DEVICE(82571EB_FIBER),
	EM_DEVICE(82571EB_SERDES),
	EM_DEVICE(82571EB_SERDES_DUAL),
	EM_DEVICE(82571EB_SERDES_QUAD),
	EM_DEVICE(82571EB_QUAD_COPPER),
	EM_DEVICE(82571EB_QUAD_COPPER_LP),
	EM_DEVICE(82571EB_QUAD_FIBER),
	EM_DEVICE(82571PT_QUAD_COPPER),

	EM_DEVICE(82572EI_COPPER),
	EM_DEVICE(82572EI_FIBER),
	EM_DEVICE(82572EI_SERDES),
	EM_DEVICE(82572EI),

	EM_DEVICE(82573E),
	EM_DEVICE(82573E_IAMT),
	EM_DEVICE(82573L),

	EM_DEVICE(80003ES2LAN_COPPER_SPT),
	EM_DEVICE(80003ES2LAN_SERDES_SPT),
	EM_DEVICE(80003ES2LAN_COPPER_DPT),
	EM_DEVICE(80003ES2LAN_SERDES_DPT),

	EM_DEVICE(ICH8_IGP_M_AMT),
	EM_DEVICE(ICH8_IGP_AMT),
	EM_DEVICE(ICH8_IGP_C),
	EM_DEVICE(ICH8_IFE),
	EM_DEVICE(ICH8_IFE_GT),
	EM_DEVICE(ICH8_IFE_G),
	EM_DEVICE(ICH8_IGP_M),

	EM_DEVICE(ICH9_IGP_M_AMT),
	EM_DEVICE(ICH9_IGP_AMT),
	EM_DEVICE(ICH9_IGP_C),
	EM_DEVICE(ICH9_IGP_M),
	EM_DEVICE(ICH9_IGP_M_V),
	EM_DEVICE(ICH9_IFE),
	EM_DEVICE(ICH9_IFE_GT),
	EM_DEVICE(ICH9_IFE_G),
	EM_DEVICE(ICH9_BM),

	EM_DEVICE(82574L),

	EM_DEVICE(ICH10_R_BM_LM),
	EM_DEVICE(ICH10_R_BM_LF),
	EM_DEVICE(ICH10_R_BM_V),
	EM_DEVICE(ICH10_D_BM_LM),
	EM_DEVICE(ICH10_D_BM_LF),

	/* required last entry */
	EM_DEVICE_NULL
};

static int	em_probe(device_t);
static int	em_attach(device_t);
static int	em_detach(device_t);
static int	em_shutdown(device_t);
static int	em_suspend(device_t);
static int	em_resume(device_t);

static void	em_init(void *);
static void	em_stop(struct adapter *);
static int	em_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	em_start(struct ifnet *);
#ifdef DEVICE_POLLING
static void	em_poll(struct ifnet *, enum poll_cmd, int);
#endif
static void	em_watchdog(struct ifnet *);
static void	em_media_status(struct ifnet *, struct ifmediareq *);
static int	em_media_change(struct ifnet *);
static void	em_timer(void *);

static void	em_intr(void *);
static void	em_rxeof(struct adapter *, int);
static void	em_txeof(struct adapter *);
static void	em_tx_purge(struct adapter *);
static void	em_enable_intr(struct adapter *);
static void	em_disable_intr(struct adapter *);

static int	em_dma_malloc(struct adapter *, bus_size_t,
		    struct em_dma_alloc *);
static void	em_dma_free(struct adapter *, struct em_dma_alloc *);
static void	em_init_tx_ring(struct adapter *);
static int	em_init_rx_ring(struct adapter *);
static int	em_create_tx_ring(struct adapter *);
static int	em_create_rx_ring(struct adapter *);
static void	em_destroy_tx_ring(struct adapter *, int);
static void	em_destroy_rx_ring(struct adapter *, int);
static int	em_newbuf(struct adapter *, int, int);
static int	em_encap(struct adapter *, struct mbuf **);
static void	em_rxcsum(struct adapter *, struct e1000_rx_desc *,
		    struct mbuf *);
static int	em_txcsum_pullup(struct adapter *, struct mbuf **);
static void	em_txcsum(struct adapter *, struct mbuf *,
		    uint32_t *, uint32_t *);

static int	em_get_hw_info(struct adapter *);
static int 	em_is_valid_eaddr(const uint8_t *);
static int	em_alloc_pci_res(struct adapter *);
static void	em_free_pci_res(struct adapter *);
static int	em_hw_init(struct adapter *);
static void	em_setup_ifp(struct adapter *);
static void	em_init_tx_unit(struct adapter *);
static void	em_init_rx_unit(struct adapter *);
static void	em_update_stats(struct adapter *);
static void	em_set_promisc(struct adapter *);
static void	em_disable_promisc(struct adapter *);
static void	em_set_multi(struct adapter *);
static void	em_update_link_status(struct adapter *);
static void	em_smartspeed(struct adapter *);

/* Hardware workarounds */
static int	em_82547_fifo_workaround(struct adapter *, int);
static void	em_82547_update_fifo_head(struct adapter *, int);
static int	em_82547_tx_fifo_reset(struct adapter *);
static void	em_82547_move_tail(void *);
static void	em_82547_move_tail_serialized(struct adapter *);
static uint32_t	em_82544_fill_desc(bus_addr_t, uint32_t, PDESC_ARRAY);

static void	em_print_debug_info(struct adapter *);
static void	em_print_nvm_info(struct adapter *);
static void	em_print_hw_stats(struct adapter *);

static int	em_sysctl_stats(SYSCTL_HANDLER_ARGS);
static int	em_sysctl_debug_info(SYSCTL_HANDLER_ARGS);
static int	em_sysctl_int_delay(SYSCTL_HANDLER_ARGS);
static int	em_sysctl_int_throttle(SYSCTL_HANDLER_ARGS);
static void	em_add_sysctl(struct adapter *adapter);
static void	em_add_int_delay_sysctl(struct adapter *, const char *,
		    const char *, struct em_int_delay_info *, int, int);

/* Management and WOL Support */
static void	em_get_mgmt(struct adapter *);
static void	em_rel_mgmt(struct adapter *);
static void	em_get_hw_control(struct adapter *);
static void	em_rel_hw_control(struct adapter *);
static void	em_enable_wol(device_t);

static device_method_t em_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		em_probe),
	DEVMETHOD(device_attach,	em_attach),
	DEVMETHOD(device_detach,	em_detach),
	DEVMETHOD(device_shutdown,	em_shutdown),
	DEVMETHOD(device_suspend,	em_suspend),
	DEVMETHOD(device_resume,	em_resume),
	{ 0, 0 }
};

static driver_t em_driver = {
	"em",
	em_methods,
	sizeof(struct adapter),
};

static devclass_t em_devclass;

DECLARE_DUMMY_MODULE(if_em);
MODULE_DEPEND(em, ig_hal, 1, 1, 1);
DRIVER_MODULE(if_em, pci, em_driver, em_devclass, 0, 0);

/*********************************************************************
 *  Tunable default values.
 *********************************************************************/

#define EM_TICKS_TO_USECS(ticks)	((1024 * (ticks) + 500) / 1000)
#define EM_USECS_TO_TICKS(usecs)	((1000 * (usecs) + 512) / 1024)

static int	em_tx_int_delay_dflt = EM_TICKS_TO_USECS(EM_TIDV);
static int	em_rx_int_delay_dflt = EM_TICKS_TO_USECS(EM_RDTR);
static int	em_tx_abs_int_delay_dflt = EM_TICKS_TO_USECS(EM_TADV);
static int	em_rx_abs_int_delay_dflt = EM_TICKS_TO_USECS(EM_RADV);
static int	em_int_throttle_ceil = EM_DEFAULT_ITR;
static int	em_rxd = EM_DEFAULT_RXD;
static int	em_txd = EM_DEFAULT_TXD;
static int	em_smart_pwr_down = FALSE;

/* Controls whether promiscuous also shows bad packets */
static int	em_debug_sbp = FALSE;

TUNABLE_INT("hw.em.tx_int_delay", &em_tx_int_delay_dflt);
TUNABLE_INT("hw.em.rx_int_delay", &em_rx_int_delay_dflt);
TUNABLE_INT("hw.em.tx_abs_int_delay", &em_tx_abs_int_delay_dflt);
TUNABLE_INT("hw.em.rx_abs_int_delay", &em_rx_abs_int_delay_dflt);
TUNABLE_INT("hw.em.int_throttle_ceil", &em_int_throttle_ceil);
TUNABLE_INT("hw.em.rxd", &em_rxd);
TUNABLE_INT("hw.em.txd", &em_txd);
TUNABLE_INT("hw.em.smart_pwr_down", &em_smart_pwr_down);
TUNABLE_INT("hw.em.sbp", &em_debug_sbp);

/* Global used in WOL setup with multiport cards */
static int	em_global_quad_port_a = 0;

/* Set this to one to display debug statistics */
static int	em_display_debug_stats = 0;

#if !defined(KTR_IF_EM)
#define KTR_IF_EM	KTR_ALL
#endif
KTR_INFO_MASTER(if_em);
KTR_INFO(KTR_IF_EM, if_em, intr_beg, 0, "intr begin", 0);
KTR_INFO(KTR_IF_EM, if_em, intr_end, 1, "intr end", 0);
KTR_INFO(KTR_IF_EM, if_em, pkt_receive, 4, "rx packet", 0);
KTR_INFO(KTR_IF_EM, if_em, pkt_txqueue, 5, "tx packet", 0);
KTR_INFO(KTR_IF_EM, if_em, pkt_txclean, 6, "tx clean", 0);
#define logif(name)	KTR_LOG(if_em_ ## name)

static int
em_probe(device_t dev)
{
	const struct em_vendor_info *ent;
	uint16_t vid, did;

	vid = pci_get_vendor(dev);
	did = pci_get_device(dev);

	for (ent = em_vendor_info_array; ent->desc != NULL; ++ent) {
		if (vid == ent->vendor_id && did == ent->device_id) {
			device_set_desc(dev, ent->desc);
			device_set_async_attach(dev, TRUE);
			return (0);
		}
	}
	return (ENXIO);
}

static int
em_attach(device_t dev)
{
	struct adapter *adapter = device_get_softc(dev);
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	int tsize, rsize;
	int error = 0;
	uint16_t eeprom_data, device_id;

	adapter->dev = adapter->osdep.dev = dev;

	callout_init(&adapter->timer);
	callout_init(&adapter->tx_fifo_timer);

	/* Determine hardware and mac info */
	error = em_get_hw_info(adapter);
	if (error) {
		device_printf(dev, "Identify hardware failed\n");
		goto fail;
	}

	/* Setup PCI resources */
	error = em_alloc_pci_res(adapter);
	if (error) {
		device_printf(dev, "Allocation of PCI resources failed\n");
		goto fail;
	}

	/*
	 * For ICH8 and family we need to map the flash memory,
	 * and this must happen after the MAC is identified.
	 */
	if (adapter->hw.mac.type == e1000_ich8lan ||
	    adapter->hw.mac.type == e1000_ich10lan ||
	    adapter->hw.mac.type == e1000_ich9lan) {
		adapter->flash_rid = EM_BAR_FLASH;

		adapter->flash = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
					&adapter->flash_rid, RF_ACTIVE);
		if (adapter->flash == NULL) {
			device_printf(dev, "Mapping of Flash failed\n");
			error = ENXIO;
			goto fail;
		}
		adapter->osdep.flash_bus_space_tag =
		    rman_get_bustag(adapter->flash);
		adapter->osdep.flash_bus_space_handle =
		    rman_get_bushandle(adapter->flash);

		/*
		 * This is used in the shared code
		 * XXX this goof is actually not used.
		 */
		adapter->hw.flash_address = (uint8_t *)adapter->flash;
	}

	/* Do Shared Code initialization */
	if (e1000_setup_init_funcs(&adapter->hw, TRUE)) {
		device_printf(dev, "Setup of Shared code failed\n");
		error = ENXIO;
		goto fail;
	}

	e1000_get_bus_info(&adapter->hw);

	/*
	 * Validate number of transmit and receive descriptors.  It
	 * must not exceed hardware maximum, and must be multiple
	 * of E1000_DBA_ALIGN.
	 */
	if ((em_txd * sizeof(struct e1000_tx_desc)) % EM_DBA_ALIGN != 0 ||
	    (adapter->hw.mac.type >= e1000_82544 && em_txd > EM_MAX_TXD) ||
	    (adapter->hw.mac.type < e1000_82544 && em_txd > EM_MAX_TXD_82543) ||
	    em_txd < EM_MIN_TXD) {
		device_printf(dev, "Using %d TX descriptors instead of %d!\n",
		    EM_DEFAULT_TXD, em_txd);
		adapter->num_tx_desc = EM_DEFAULT_TXD;
	} else {
		adapter->num_tx_desc = em_txd;
	}
	if ((em_rxd * sizeof(struct e1000_rx_desc)) % EM_DBA_ALIGN != 0 ||
	    (adapter->hw.mac.type >= e1000_82544 && em_rxd > EM_MAX_RXD) ||
	    (adapter->hw.mac.type < e1000_82544 && em_rxd > EM_MAX_RXD_82543) ||
	    em_rxd < EM_MIN_RXD) {
		device_printf(dev, "Using %d RX descriptors instead of %d!\n",
		    EM_DEFAULT_RXD, em_rxd);
		adapter->num_rx_desc = EM_DEFAULT_RXD;
	} else {
		adapter->num_rx_desc = em_rxd;
	}

	adapter->hw.mac.autoneg = DO_AUTO_NEG;
	adapter->hw.phy.autoneg_wait_to_complete = FALSE;
	adapter->hw.phy.autoneg_advertised = AUTONEG_ADV_DEFAULT;
	adapter->rx_buffer_len = MCLBYTES;

	/*
	 * Interrupt throttle rate
	 */
	if (em_int_throttle_ceil == 0) {
		adapter->int_throttle_ceil = 0;
	} else {
		int throttle = em_int_throttle_ceil;

		if (throttle < 0)
			throttle = EM_DEFAULT_ITR;

		/* Recalculate the tunable value to get the exact frequency. */
		throttle = 1000000000 / 256 / throttle;
		adapter->int_throttle_ceil = 1000000000 / 256 / throttle;
	}

	e1000_init_script_state_82541(&adapter->hw, TRUE);
	e1000_set_tbi_compatibility_82543(&adapter->hw, TRUE);

	/* Copper options */
	if (adapter->hw.phy.media_type == e1000_media_type_copper) {
		adapter->hw.phy.mdix = AUTO_ALL_MODES;
		adapter->hw.phy.disable_polarity_correction = FALSE;
		adapter->hw.phy.ms_type = EM_MASTER_SLAVE;
	}

	/* Set the frame limits assuming standard ethernet sized frames. */
	adapter->max_frame_size = ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN;
	adapter->min_frame_size = ETH_ZLEN + ETHER_CRC_LEN;

	/* This controls when hardware reports transmit completion status. */
	adapter->hw.mac.report_tx_early = 1;

	/*
	 * Create top level busdma tag
	 */
	error = bus_dma_tag_create(NULL, 1, 0,
			BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
			NULL, NULL,
			BUS_SPACE_MAXSIZE_32BIT, 0, BUS_SPACE_MAXSIZE_32BIT,
			0, &adapter->parent_dtag);
	if (error) {
		device_printf(dev, "could not create top level DMA tag\n");
		goto fail;
	}

	/*
	 * Allocate Transmit Descriptor ring
	 */
	tsize = roundup2(adapter->num_tx_desc * sizeof(struct e1000_tx_desc),
			 EM_DBA_ALIGN);
	error = em_dma_malloc(adapter, tsize, &adapter->txdma);
	if (error) {
		device_printf(dev, "Unable to allocate tx_desc memory\n");
		goto fail;
	}
	adapter->tx_desc_base = adapter->txdma.dma_vaddr;

	/*
	 * Allocate Receive Descriptor ring
	 */
	rsize = roundup2(adapter->num_rx_desc * sizeof(struct e1000_rx_desc),
			 EM_DBA_ALIGN);
	error = em_dma_malloc(adapter, rsize, &adapter->rxdma);
	if (error) {
		device_printf(dev, "Unable to allocate rx_desc memory\n");
		goto fail;
	}
	adapter->rx_desc_base = adapter->rxdma.dma_vaddr;

	/* Make sure we have a good EEPROM before we read from it */
	if (e1000_validate_nvm_checksum(&adapter->hw) < 0) {
		/*
		 * Some PCI-E parts fail the first check due to
		 * the link being in sleep state, call it again,
		 * if it fails a second time its a real issue.
		 */
		if (e1000_validate_nvm_checksum(&adapter->hw) < 0) {
			device_printf(dev,
			    "The EEPROM Checksum Is Not Valid\n");
			error = EIO;
			goto fail;
		}
	}

	/* Initialize the hardware */
	error = em_hw_init(adapter);
	if (error) {
		device_printf(dev, "Unable to initialize the hardware\n");
		goto fail;
	}

	/* Copy the permanent MAC address out of the EEPROM */
	if (e1000_read_mac_addr(&adapter->hw) < 0) {
		device_printf(dev, "EEPROM read error while reading MAC"
		    " address\n");
		error = EIO;
		goto fail;
	}
	if (!em_is_valid_eaddr(adapter->hw.mac.addr)) {
		device_printf(dev, "Invalid MAC address\n");
		error = EIO;
		goto fail;
	}

	/* Allocate transmit descriptors and buffers */
	error = em_create_tx_ring(adapter);
	if (error) {
		device_printf(dev, "Could not setup transmit structures\n");
		goto fail;
	}

	/* Allocate receive descriptors and buffers */
	error = em_create_rx_ring(adapter);
	if (error) {
		device_printf(dev, "Could not setup receive structures\n");
		goto fail;
	}

	/* Manually turn off all interrupts */
	E1000_WRITE_REG(&adapter->hw, E1000_IMC, 0xffffffff);

	/* Setup OS specific network interface */
	em_setup_ifp(adapter);

	/* Add sysctl tree, must after em_setup_ifp() */
	em_add_sysctl(adapter);

	/* Initialize statistics */
	em_update_stats(adapter);

	adapter->hw.mac.get_link_status = 1;
	em_update_link_status(adapter);

	/* Indicate SOL/IDER usage */
	if (e1000_check_reset_block(&adapter->hw)) {
		device_printf(dev,
		    "PHY reset is blocked due to SOL/IDER session.\n");
	}

	/* Determine if we have to control management hardware */
	adapter->has_manage = e1000_enable_mng_pass_thru(&adapter->hw);

	/*
	 * Setup Wake-on-Lan
	 */
	switch (adapter->hw.mac.type) {
	case e1000_82542:
	case e1000_82543:
		break;

	case e1000_82546:
	case e1000_82546_rev_3:
	case e1000_82571:
	case e1000_80003es2lan:
		if (adapter->hw.bus.func == 1) {
			e1000_read_nvm(&adapter->hw,
			    NVM_INIT_CONTROL3_PORT_B, 1, &eeprom_data);
		} else {
			e1000_read_nvm(&adapter->hw,
			    NVM_INIT_CONTROL3_PORT_A, 1, &eeprom_data);
		}
		eeprom_data &= EM_EEPROM_APME;
		break;

	default:
		/* APME bit in EEPROM is mapped to WUC.APME */
		eeprom_data =
		    E1000_READ_REG(&adapter->hw, E1000_WUC) & E1000_WUC_APME;
		break;
	}
	if (eeprom_data)
		adapter->wol = E1000_WUFC_MAG;
	/*
         * We have the eeprom settings, now apply the special cases
         * where the eeprom may be wrong or the board won't support
         * wake on lan on a particular port
	 */
	device_id = pci_get_device(dev);
        switch (device_id) {
	case E1000_DEV_ID_82546GB_PCIE:
		adapter->wol = 0;
		break;

	case E1000_DEV_ID_82546EB_FIBER:
	case E1000_DEV_ID_82546GB_FIBER:
	case E1000_DEV_ID_82571EB_FIBER:
		/*
		 * Wake events only supported on port A for dual fiber
		 * regardless of eeprom setting
		 */
		if (E1000_READ_REG(&adapter->hw, E1000_STATUS) &
		    E1000_STATUS_FUNC_1)
			adapter->wol = 0;
		break;

	case E1000_DEV_ID_82546GB_QUAD_COPPER_KSP3:
	case E1000_DEV_ID_82571EB_QUAD_COPPER:
	case E1000_DEV_ID_82571EB_QUAD_FIBER:
	case E1000_DEV_ID_82571EB_QUAD_COPPER_LP:
                /* if quad port adapter, disable WoL on all but port A */
		if (em_global_quad_port_a != 0)
			adapter->wol = 0;
		/* Reset for multiple quad port adapters */
		if (++em_global_quad_port_a == 4)
			em_global_quad_port_a = 0;
                break;
	}

	/* XXX disable wol */
	adapter->wol = 0;

	/* Do we need workaround for 82544 PCI-X adapter? */
	if (adapter->hw.bus.type == e1000_bus_type_pcix &&
	    adapter->hw.mac.type == e1000_82544)
		adapter->pcix_82544 = TRUE;
	else
		adapter->pcix_82544 = FALSE;

	if (adapter->pcix_82544) {
		/*
		 * 82544 on PCI-X may split one TX segment
		 * into two TX descs, so we double its number
		 * of spare TX desc here.
		 */
		adapter->spare_tx_desc = 2 * EM_TX_SPARE;
	} else {
		adapter->spare_tx_desc = EM_TX_SPARE;
	}

	error = bus_setup_intr(dev, adapter->intr_res, INTR_MPSAFE,
			       em_intr, adapter, &adapter->intr_tag,
			       ifp->if_serializer);
	if (error) {
		device_printf(dev, "Failed to register interrupt handler");
		ether_ifdetach(&adapter->arpcom.ac_if);
		goto fail;
	}

	ifp->if_cpuid = ithread_cpuid(rman_get_start(adapter->intr_res));
	KKASSERT(ifp->if_cpuid >= 0 && ifp->if_cpuid < ncpus);
	return (0);
fail:
	em_detach(dev);
	return (error);
}

static int
em_detach(device_t dev)
{
	struct adapter *adapter = device_get_softc(dev);

	if (device_is_attached(dev)) {
		struct ifnet *ifp = &adapter->arpcom.ac_if;

		lwkt_serialize_enter(ifp->if_serializer);

		adapter->in_detach = 1;
		em_stop(adapter);

		e1000_phy_hw_reset(&adapter->hw);

		em_rel_mgmt(adapter);

		if ((adapter->hw.mac.type == e1000_82573 ||
		     adapter->hw.mac.type == e1000_ich8lan ||
		     adapter->hw.mac.type == e1000_ich10lan ||
		     adapter->hw.mac.type == e1000_ich9lan) &&
		    e1000_check_mng_mode(&adapter->hw))
			em_rel_hw_control(adapter);

		if (adapter->wol) {
			E1000_WRITE_REG(&adapter->hw, E1000_WUC,
					E1000_WUC_PME_EN);
			E1000_WRITE_REG(&adapter->hw, E1000_WUFC, adapter->wol);
			em_enable_wol(dev);
		}

		bus_teardown_intr(dev, adapter->intr_res, adapter->intr_tag);

		lwkt_serialize_exit(ifp->if_serializer);

		ether_ifdetach(ifp);
	}
	bus_generic_detach(dev);

	em_free_pci_res(adapter);

	em_destroy_tx_ring(adapter, adapter->num_tx_desc);
	em_destroy_rx_ring(adapter, adapter->num_rx_desc);

	/* Free Transmit Descriptor ring */
	if (adapter->tx_desc_base)
		em_dma_free(adapter, &adapter->txdma);

	/* Free Receive Descriptor ring */
	if (adapter->rx_desc_base)
		em_dma_free(adapter, &adapter->rxdma);

	/* Free top level busdma tag */
	if (adapter->parent_dtag != NULL)
		bus_dma_tag_destroy(adapter->parent_dtag);

	/* Free sysctl tree */
	if (adapter->sysctl_tree != NULL)
		sysctl_ctx_free(&adapter->sysctl_ctx);

	return (0);
}

static int
em_shutdown(device_t dev)
{
	return em_suspend(dev);
}

static int
em_suspend(device_t dev)
{
	struct adapter *adapter = device_get_softc(dev);
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	em_stop(adapter);

	em_rel_mgmt(adapter);

        if ((adapter->hw.mac.type == e1000_82573 ||
             adapter->hw.mac.type == e1000_ich8lan ||
             adapter->hw.mac.type == e1000_ich10lan ||
             adapter->hw.mac.type == e1000_ich9lan) &&
            e1000_check_mng_mode(&adapter->hw))
                em_rel_hw_control(adapter);

        if (adapter->wol) {
		E1000_WRITE_REG(&adapter->hw, E1000_WUC, E1000_WUC_PME_EN);
		E1000_WRITE_REG(&adapter->hw, E1000_WUFC, adapter->wol);
		em_enable_wol(dev);
        }

	lwkt_serialize_exit(ifp->if_serializer);

	return bus_generic_suspend(dev);
}

static int
em_resume(device_t dev)
{
	struct adapter *adapter = device_get_softc(dev);
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	em_init(adapter);
	em_get_mgmt(adapter);
	if_devstart(ifp);

	lwkt_serialize_exit(ifp->if_serializer);

	return bus_generic_resume(dev);
}

static void
em_start(struct ifnet *ifp)
{
	struct adapter *adapter = ifp->if_softc;
	struct mbuf *m_head;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	if (!adapter->link_active) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	while (!ifq_is_empty(&ifp->if_snd)) {
		/*
		 * Force a cleanup if number of TX descriptors
		 * available hits the threshold
		 */
		if (adapter->num_tx_desc_avail <= EM_TX_CLEANUP_THRESHOLD) {
			em_txeof(adapter);

			/* Now do we at least have a minimal? */
			if (EM_IS_OACTIVE(adapter)) {
				adapter->no_tx_desc_avail1++;
				ifp->if_flags |= IFF_OACTIVE;
				break;
			}
		}

		logif(pkt_txqueue);
		m_head = ifq_dequeue(&ifp->if_snd, NULL);
		if (m_head == NULL)
			break;

		if (em_encap(adapter, &m_head)) {
			ifp->if_oerrors++;
			if (adapter->num_tx_desc_avail ==
			    adapter->num_tx_desc) {
				continue;
			} else {
				ifp->if_flags |= IFF_OACTIVE;
				break;
			}
		}

		/* Send a copy of the frame to the BPF listener */
		ETHER_BPF_MTAP(ifp, m_head);

		/* Set timeout in case hardware has problems transmitting. */
		ifp->if_timer = EM_TX_TIMEOUT;
	}
}

static int
em_ioctl(struct ifnet *ifp, u_long command, caddr_t data, struct ucred *cr)
{
	struct adapter *adapter = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	uint16_t eeprom_data = 0;
	int max_frame_size, mask, reinit;
	int error = 0;

	if (adapter->in_detach)
		return (error);

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch (command) {
	case SIOCSIFMTU:
		switch (adapter->hw.mac.type) {
		case e1000_82573:
			/*
			 * 82573 only supports jumbo frames
			 * if ASPM is disabled.
			 */
			e1000_read_nvm(&adapter->hw,
			    NVM_INIT_3GIO_3, 1, &eeprom_data);
			if (eeprom_data & NVM_WORD1A_ASPM_MASK) {
				max_frame_size = ETHER_MAX_LEN;
				break;
			}
			/* FALL THROUGH */

		/* Limit Jumbo Frame size */
		case e1000_82571:
		case e1000_82572:
		case e1000_ich9lan:
		case e1000_ich10lan:
		case e1000_82574:
		case e1000_80003es2lan:
			max_frame_size = 9234;
			break;

		/* Adapters that do not support jumbo frames */
		case e1000_82542:
		case e1000_ich8lan:
			max_frame_size = ETHER_MAX_LEN;
			break;

		default:
			max_frame_size = MAX_JUMBO_FRAME_SIZE;
			break;
		}
		if (ifr->ifr_mtu > max_frame_size - ETHER_HDR_LEN -
		    ETHER_CRC_LEN) {
			error = EINVAL;
			break;
		}

		ifp->if_mtu = ifr->ifr_mtu;
		adapter->max_frame_size =
		    ifp->if_mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

		if (ifp->if_flags & IFF_RUNNING)
			em_init(adapter);
		break;

	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_flags & IFF_RUNNING)) {
				if ((ifp->if_flags ^ adapter->if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					em_disable_promisc(adapter);
					em_set_promisc(adapter);
				}
			} else {
				em_init(adapter);
			}
		} else if (ifp->if_flags & IFF_RUNNING) {
			em_stop(adapter);
		}
		adapter->if_flags = ifp->if_flags;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_flags & IFF_RUNNING) {
			em_disable_intr(adapter);
			em_set_multi(adapter);
			if (adapter->hw.mac.type == e1000_82542 &&
			    adapter->hw.revision_id == E1000_REVISION_2)
				em_init_rx_unit(adapter);
#ifdef DEVICE_POLLING
			if (!(ifp->if_flags & IFF_POLLING))
#endif
				em_enable_intr(adapter);
		}
		break;

	case SIOCSIFMEDIA:
		/* Check SOL/IDER usage */
		if (e1000_check_reset_block(&adapter->hw)) {
			device_printf(adapter->dev, "Media change is"
			    " blocked due to SOL/IDER session.\n");
			break;
		}
		/* FALL THROUGH */

	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &adapter->media, command);
		break;

	case SIOCSIFCAP:
		reinit = 0;
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if (mask & IFCAP_HWCSUM) {
			ifp->if_capenable ^= (mask & IFCAP_HWCSUM);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTAGGING) {
			ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;
			reinit = 1;
		}
		if (reinit && (ifp->if_flags & IFF_RUNNING))
			em_init(adapter);
		break;

	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}
	return (error);
}

static void
em_watchdog(struct ifnet *ifp)
{
	struct adapter *adapter = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	/*
	 * The timer is set to 5 every time start queues a packet.
	 * Then txeof keeps resetting it as long as it cleans at
	 * least one descriptor.
	 * Finally, anytime all descriptors are clean the timer is
	 * set to 0.
	 */

	/*
	 * If we are in this routine because of pause frames, then
	 * don't reset the hardware.
	 */
	if (E1000_READ_REG(&adapter->hw, E1000_STATUS) &
	    E1000_STATUS_TXOFF) {
		ifp->if_timer = EM_TX_TIMEOUT;
		return;
	}

	if (e1000_check_for_link(&adapter->hw) == 0)
		if_printf(ifp, "watchdog timeout -- resetting\n");

	ifp->if_oerrors++;
	adapter->watchdog_events++;

	em_init(adapter);

	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

static void
em_init(void *xsc)
{
	struct adapter *adapter = xsc;
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	device_t dev = adapter->dev;
	uint32_t pba;

	ASSERT_SERIALIZED(ifp->if_serializer);

	em_stop(adapter);

	/*
	 * Packet Buffer Allocation (PBA)
	 * Writing PBA sets the receive portion of the buffer
	 * the remainder is used for the transmit buffer.
	 *
	 * Devices before the 82547 had a Packet Buffer of 64K.
	 *   Default allocation: PBA=48K for Rx, leaving 16K for Tx.
	 * After the 82547 the buffer was reduced to 40K.
	 *   Default allocation: PBA=30K for Rx, leaving 10K for Tx.
	 *   Note: default does not leave enough room for Jumbo Frame >10k.
	 */
	switch (adapter->hw.mac.type) {
	case e1000_82547:
	case e1000_82547_rev_2: /* 82547: Total Packet Buffer is 40K */
		if (adapter->max_frame_size > 8192)
			pba = E1000_PBA_22K; /* 22K for Rx, 18K for Tx */
		else
			pba = E1000_PBA_30K; /* 30K for Rx, 10K for Tx */
		adapter->tx_fifo_head = 0;
		adapter->tx_head_addr = pba << EM_TX_HEAD_ADDR_SHIFT;
		adapter->tx_fifo_size =
		    (E1000_PBA_40K - pba) << EM_PBA_BYTES_SHIFT;
		break;

	/* Total Packet Buffer on these is 48K */
	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
		pba = E1000_PBA_32K; /* 32K for Rx, 16K for Tx */
		break;

	case e1000_82573: /* 82573: Total Packet Buffer is 32K */
		pba = E1000_PBA_12K; /* 12K for Rx, 20K for Tx */
		break;

	case e1000_82574:
		pba = E1000_PBA_20K; /* 20K for Rx, 20K for Tx */
		break;

	case e1000_ich9lan:
	case e1000_ich10lan:
#define E1000_PBA_10K	0x000A
		pba = E1000_PBA_10K;
		break;

	case e1000_ich8lan:
		pba = E1000_PBA_8K;
		break;

	default:
		/* Devices before 82547 had a Packet Buffer of 64K.   */
		if (adapter->max_frame_size > 8192)
			pba = E1000_PBA_40K; /* 40K for Rx, 24K for Tx */
		else
			pba = E1000_PBA_48K; /* 48K for Rx, 16K for Tx */
	}

	E1000_WRITE_REG(&adapter->hw, E1000_PBA, pba);
	
	/* Get the latest mac address, User can use a LAA */
        bcopy(IF_LLADDR(ifp), adapter->hw.mac.addr, ETHER_ADDR_LEN);

	/* Put the address into the Receive Address Array */
	e1000_rar_set(&adapter->hw, adapter->hw.mac.addr, 0);

	/*
	 * With the 82571 adapter, RAR[0] may be overwritten
	 * when the other port is reset, we make a duplicate
	 * in RAR[14] for that eventuality, this assures
	 * the interface continues to function.
	 */
	if (adapter->hw.mac.type == e1000_82571) {
		e1000_set_laa_state_82571(&adapter->hw, TRUE);
		e1000_rar_set(&adapter->hw, adapter->hw.mac.addr,
		    E1000_RAR_ENTRIES - 1);
	}

	/* Initialize the hardware */
	if (em_hw_init(adapter)) {
		device_printf(dev, "Unable to initialize the hardware\n");
		/* XXX em_stop()? */
		return;
	}
	em_update_link_status(adapter);

	/* Setup VLAN support, basic and offload if available */
	E1000_WRITE_REG(&adapter->hw, E1000_VET, ETHERTYPE_VLAN);

	if (ifp->if_capenable & IFCAP_VLAN_HWTAGGING) {
		uint32_t ctrl;

		ctrl = E1000_READ_REG(&adapter->hw, E1000_CTRL);
		ctrl |= E1000_CTRL_VME;
		E1000_WRITE_REG(&adapter->hw, E1000_CTRL, ctrl);
	}

	/* Set hardware offload abilities */
	if (ifp->if_capenable & IFCAP_TXCSUM)
		ifp->if_hwassist = EM_CSUM_FEATURES;
	else
		ifp->if_hwassist = 0;

	/* Configure for OS presence */
	em_get_mgmt(adapter);

	/* Prepare transmit descriptors and buffers */
	em_init_tx_ring(adapter);
	em_init_tx_unit(adapter);

	/* Setup Multicast table */
	em_set_multi(adapter);

	/* Prepare receive descriptors and buffers */
	if (em_init_rx_ring(adapter)) {
		device_printf(dev, "Could not setup receive structures\n");
		em_stop(adapter);
		return;
	}
	em_init_rx_unit(adapter);

	/* Don't lose promiscuous settings */
	em_set_promisc(adapter);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	callout_reset(&adapter->timer, hz, em_timer, adapter);
	e1000_clear_hw_cntrs_base_generic(&adapter->hw);

	/* MSI/X configuration for 82574 */
	if (adapter->hw.mac.type == e1000_82574) {
		int tmp;

		tmp = E1000_READ_REG(&adapter->hw, E1000_CTRL_EXT);
		tmp |= E1000_CTRL_EXT_PBA_CLR;
		E1000_WRITE_REG(&adapter->hw, E1000_CTRL_EXT, tmp);
		/*
		 * Set the IVAR - interrupt vector routing.
		 * Each nibble represents a vector, high bit
		 * is enable, other 3 bits are the MSIX table
		 * entry, we map RXQ0 to 0, TXQ0 to 1, and
		 * Link (other) to 2, hence the magic number.
		 */
		E1000_WRITE_REG(&adapter->hw, E1000_IVAR, 0x800A0908);
	}

#ifdef DEVICE_POLLING
	/*
	 * Only enable interrupts if we are not polling, make sure
	 * they are off otherwise.
	 */
	if (ifp->if_flags & IFF_POLLING)
		em_disable_intr(adapter);
	else
#endif /* DEVICE_POLLING */
		em_enable_intr(adapter);

	/* Don't reset the phy next time init gets called */
	adapter->hw.phy.reset_disable = TRUE;
}

#ifdef DEVICE_POLLING

static void
em_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct adapter *adapter = ifp->if_softc;
	uint32_t reg_icr;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch (cmd) {
	case POLL_REGISTER:
		em_disable_intr(adapter);
		break;

	case POLL_DEREGISTER:
		em_enable_intr(adapter);
		break;

	case POLL_AND_CHECK_STATUS:
		reg_icr = E1000_READ_REG(&adapter->hw, E1000_ICR);
		if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
			callout_stop(&adapter->timer);
			adapter->hw.mac.get_link_status = 1;
			em_update_link_status(adapter);
			callout_reset(&adapter->timer, hz, em_timer, adapter);
		}
		/* FALL THROUGH */
	case POLL_ONLY:
		if (ifp->if_flags & IFF_RUNNING) {
			em_rxeof(adapter, count);
			em_txeof(adapter);

			if (!ifq_is_empty(&ifp->if_snd))
				if_devstart(ifp);
		}
		break;
	}
}

#endif /* DEVICE_POLLING */

static void
em_intr(void *xsc)
{
	struct adapter *adapter = xsc;
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	uint32_t reg_icr;

	logif(intr_beg);
	ASSERT_SERIALIZED(ifp->if_serializer);

	reg_icr = E1000_READ_REG(&adapter->hw, E1000_ICR);

	if ((adapter->hw.mac.type >= e1000_82571 &&
	     (reg_icr & E1000_ICR_INT_ASSERTED) == 0) ||
	    reg_icr == 0) {
		logif(intr_end);
		return;
	}

	/*
	 * XXX: some laptops trigger several spurious interrupts
	 * on em(4) when in the resume cycle. The ICR register
	 * reports all-ones value in this case. Processing such
	 * interrupts would lead to a freeze. I don't know why.
	 */
	if (reg_icr == 0xffffffff) {
		logif(intr_end);
		return;
	}

	if (ifp->if_flags & IFF_RUNNING) {
		em_rxeof(adapter, -1);
		em_txeof(adapter);
	}

	/* Link status change */
	if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		callout_stop(&adapter->timer);
		adapter->hw.mac.get_link_status = 1;
		em_update_link_status(adapter);

		/* Deal with TX cruft when link lost */
		em_tx_purge(adapter);

		callout_reset(&adapter->timer, hz, em_timer, adapter);
	}

	if (reg_icr & E1000_ICR_RXO)
		adapter->rx_overruns++;

	if ((ifp->if_flags & IFF_RUNNING) && !ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);

	logif(intr_end);
}

static void
em_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct adapter *adapter = ifp->if_softc;
	u_char fiber_type = IFM_1000_SX;

	ASSERT_SERIALIZED(ifp->if_serializer);

	em_update_link_status(adapter);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

	if (!adapter->link_active)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;

	if (adapter->hw.phy.media_type == e1000_media_type_fiber ||
	    adapter->hw.phy.media_type == e1000_media_type_internal_serdes) {
		if (adapter->hw.mac.type == e1000_82545)
			fiber_type = IFM_1000_LX;
		ifmr->ifm_active |= fiber_type | IFM_FDX;
	} else {
		switch (adapter->link_speed) {
		case 10:
			ifmr->ifm_active |= IFM_10_T;
			break;
		case 100:
			ifmr->ifm_active |= IFM_100_TX;
			break;

		case 1000:
			ifmr->ifm_active |= IFM_1000_T;
			break;
		}
		if (adapter->link_duplex == FULL_DUPLEX)
			ifmr->ifm_active |= IFM_FDX;
		else
			ifmr->ifm_active |= IFM_HDX;
	}
}

static int
em_media_change(struct ifnet *ifp)
{
	struct adapter *adapter = ifp->if_softc;
	struct ifmedia *ifm = &adapter->media;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if (IFM_TYPE(ifm->ifm_media) != IFM_ETHER)
		return (EINVAL);

	switch (IFM_SUBTYPE(ifm->ifm_media)) {
	case IFM_AUTO:
		adapter->hw.mac.autoneg = DO_AUTO_NEG;
		adapter->hw.phy.autoneg_advertised = AUTONEG_ADV_DEFAULT;
		break;

	case IFM_1000_LX:
	case IFM_1000_SX:
	case IFM_1000_T:
		adapter->hw.mac.autoneg = DO_AUTO_NEG;
		adapter->hw.phy.autoneg_advertised = ADVERTISE_1000_FULL;
		break;

	case IFM_100_TX:
		adapter->hw.mac.autoneg = FALSE;
		adapter->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			adapter->hw.mac.forced_speed_duplex = ADVERTISE_100_FULL;
		else
			adapter->hw.mac.forced_speed_duplex = ADVERTISE_100_HALF;
		break;

	case IFM_10_T:
		adapter->hw.mac.autoneg = FALSE;
		adapter->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			adapter->hw.mac.forced_speed_duplex = ADVERTISE_10_FULL;
		else
			adapter->hw.mac.forced_speed_duplex = ADVERTISE_10_HALF;
		break;

	default:
		if_printf(ifp, "Unsupported media type\n");
		break;
	}

	/*
	 * As the speed/duplex settings my have changed we need to
	 * reset the PHY.
	 */
	adapter->hw.phy.reset_disable = FALSE;

	em_init(adapter);

	return (0);
}

static int
em_encap(struct adapter *adapter, struct mbuf **m_headp)
{
	bus_dma_segment_t segs[EM_MAX_SCATTER];
	bus_dmamap_t map;
	struct em_buffer *tx_buffer, *tx_buffer_mapped;
	struct e1000_tx_desc *ctxd = NULL;
	struct mbuf *m_head = *m_headp;
	uint32_t txd_upper, txd_lower, txd_used;
	int maxsegs, nsegs, i, j, first, last = 0, error;

	if (__predict_false(m_head->m_len < EM_TXCSUM_MINHL) &&
	    (m_head->m_flags & EM_CSUM_FEATURES)) {
		/*
		 * Make sure that ethernet header and ip.ip_hl are in
		 * contiguous memory, since if TXCSUM is enabled, later
		 * TX context descriptor's setup need to access ip.ip_hl.
		 */
		error = em_txcsum_pullup(adapter, m_headp);
		if (error) {
			KKASSERT(*m_headp == NULL);
			return error;
		}
		m_head = *m_headp;
	}

	txd_upper = txd_lower = 0;
	txd_used = 0;

	/*
	 * Capture the first descriptor index, this descriptor
	 * will have the index of the EOP which is the only one
	 * that now gets a DONE bit writeback.
	 */
	first = adapter->next_avail_tx_desc;
	tx_buffer = &adapter->tx_buffer_area[first];
	tx_buffer_mapped = tx_buffer;
	map = tx_buffer->map;

	maxsegs = adapter->num_tx_desc_avail - EM_TX_RESERVED;
	KASSERT(maxsegs >= adapter->spare_tx_desc,
		("not enough spare TX desc\n"));
	if (adapter->pcix_82544) {
		/* Half it; see the comment in em_attach() */
		maxsegs >>= 1;
	}
	if (maxsegs > EM_MAX_SCATTER)
		maxsegs = EM_MAX_SCATTER;

	error = bus_dmamap_load_mbuf_defrag(adapter->txtag, map, m_headp,
			segs, maxsegs, &nsegs, BUS_DMA_NOWAIT);
	if (error) {
		if (error == ENOBUFS)
			adapter->mbuf_alloc_failed++;
		else
			adapter->no_tx_dma_setup++;

		m_freem(*m_headp);
		*m_headp = NULL;
		return error;
	}
        bus_dmamap_sync(adapter->txtag, map, BUS_DMASYNC_PREWRITE);

	m_head = *m_headp;

	if (m_head->m_pkthdr.csum_flags & EM_CSUM_FEATURES) {
		/* TX csum offloading will consume one TX desc */
		em_txcsum(adapter, m_head, &txd_upper, &txd_lower);
	}
	i = adapter->next_avail_tx_desc;

	/* Set up our transmit descriptors */
	for (j = 0; j < nsegs; j++) {
		/* If adapter is 82544 and on PCIX bus */
		if(adapter->pcix_82544) {
			DESC_ARRAY desc_array;
			uint32_t array_elements, counter;

			/*
			 * Check the Address and Length combination and
			 * split the data accordingly
			 */
			array_elements = em_82544_fill_desc(segs[j].ds_addr,
						segs[j].ds_len, &desc_array);
			for (counter = 0; counter < array_elements; counter++) {
				KKASSERT(txd_used < adapter->num_tx_desc_avail);

				tx_buffer = &adapter->tx_buffer_area[i];
				ctxd = &adapter->tx_desc_base[i];

				ctxd->buffer_addr = htole64(
				    desc_array.descriptor[counter].address);
				ctxd->lower.data = htole32(
				    adapter->txd_cmd | txd_lower |
				    desc_array.descriptor[counter].length);
				ctxd->upper.data = htole32(txd_upper);

				last = i;
				if (++i == adapter->num_tx_desc)
					i = 0;

				tx_buffer->m_head = NULL;
				tx_buffer->next_eop = -1;
				txd_used++;
                        }
		} else {
			tx_buffer = &adapter->tx_buffer_area[i];
			ctxd = &adapter->tx_desc_base[i];

			ctxd->buffer_addr = htole64(segs[j].ds_addr);
			ctxd->lower.data = htole32(adapter->txd_cmd |
						   txd_lower | segs[j].ds_len);
			ctxd->upper.data = htole32(txd_upper);

			last = i;
			if (++i == adapter->num_tx_desc)
				i = 0;

			tx_buffer->m_head = NULL;
			tx_buffer->next_eop = -1;
		}
	}

	adapter->next_avail_tx_desc = i;
	if (adapter->pcix_82544) {
		KKASSERT(adapter->num_tx_desc_avail > txd_used);
		adapter->num_tx_desc_avail -= txd_used;
	} else {
		KKASSERT(adapter->num_tx_desc_avail > nsegs);
		adapter->num_tx_desc_avail -= nsegs;
	}

        /* Handle VLAN tag */
	if (m_head->m_flags & M_VLANTAG) {
		/* Set the vlan id. */
		ctxd->upper.fields.special =
		    htole16(m_head->m_pkthdr.ether_vlantag);

		/* Tell hardware to add tag */
		ctxd->lower.data |= htole32(E1000_TXD_CMD_VLE);
	}

	tx_buffer->m_head = m_head;
	tx_buffer_mapped->map = tx_buffer->map;
	tx_buffer->map = map;

	/*
	 * Last Descriptor of Packet needs End Of Packet (EOP)
	 * and Report Status (RS)
	 */
	ctxd->lower.data |= htole32(E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS);

	/*
	 * Keep track in the first buffer which descriptor will be
	 * written back
	 */
	tx_buffer = &adapter->tx_buffer_area[first];
	tx_buffer->next_eop = last;

	/*
	 * Advance the Transmit Descriptor Tail (TDT), this tells the E1000
	 * that this frame is available to transmit.
	 */
	if (adapter->hw.mac.type == e1000_82547 &&
	    adapter->link_duplex == HALF_DUPLEX) {
		em_82547_move_tail_serialized(adapter);
	} else {
		E1000_WRITE_REG(&adapter->hw, E1000_TDT(0), i);
		if (adapter->hw.mac.type == e1000_82547) {
			em_82547_update_fifo_head(adapter,
			    m_head->m_pkthdr.len);
		}
	}
	return (0);
}

/*
 * 82547 workaround to avoid controller hang in half-duplex environment.
 * The workaround is to avoid queuing a large packet that would span
 * the internal Tx FIFO ring boundary.  We need to reset the FIFO pointers
 * in this case.  We do that only when FIFO is quiescent.
 */
static void
em_82547_move_tail_serialized(struct adapter *adapter)
{
	struct e1000_tx_desc *tx_desc;
	uint16_t hw_tdt, sw_tdt, length = 0;
	bool eop = 0;

	ASSERT_SERIALIZED(adapter->arpcom.ac_if.if_serializer);

	hw_tdt = E1000_READ_REG(&adapter->hw, E1000_TDT(0));
	sw_tdt = adapter->next_avail_tx_desc;

	while (hw_tdt != sw_tdt) {
		tx_desc = &adapter->tx_desc_base[hw_tdt];
		length += tx_desc->lower.flags.length;
		eop = tx_desc->lower.data & E1000_TXD_CMD_EOP;
		if (++hw_tdt == adapter->num_tx_desc)
			hw_tdt = 0;

		if (eop) {
			if (em_82547_fifo_workaround(adapter, length)) {
				adapter->tx_fifo_wrk_cnt++;
				callout_reset(&adapter->tx_fifo_timer, 1,
					em_82547_move_tail, adapter);
				break;
			}
			E1000_WRITE_REG(&adapter->hw, E1000_TDT(0), hw_tdt);
			em_82547_update_fifo_head(adapter, length);
			length = 0;
		}
	}
}

static void
em_82547_move_tail(void *xsc)
{
	struct adapter *adapter = xsc;
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);
	em_82547_move_tail_serialized(adapter);
	lwkt_serialize_exit(ifp->if_serializer);
}

static int
em_82547_fifo_workaround(struct adapter *adapter, int len)
{	
	int fifo_space, fifo_pkt_len;

	fifo_pkt_len = roundup2(len + EM_FIFO_HDR, EM_FIFO_HDR);

	if (adapter->link_duplex == HALF_DUPLEX) {
		fifo_space = adapter->tx_fifo_size - adapter->tx_fifo_head;

		if (fifo_pkt_len >= (EM_82547_PKT_THRESH + fifo_space)) {
			if (em_82547_tx_fifo_reset(adapter))
				return (0);
			else
				return (1);
		}
	}
	return (0);
}

static void
em_82547_update_fifo_head(struct adapter *adapter, int len)
{
	int fifo_pkt_len = roundup2(len + EM_FIFO_HDR, EM_FIFO_HDR);

	/* tx_fifo_head is always 16 byte aligned */
	adapter->tx_fifo_head += fifo_pkt_len;
	if (adapter->tx_fifo_head >= adapter->tx_fifo_size)
		adapter->tx_fifo_head -= adapter->tx_fifo_size;
}

static int
em_82547_tx_fifo_reset(struct adapter *adapter)
{
	uint32_t tctl;

	if ((E1000_READ_REG(&adapter->hw, E1000_TDT(0)) ==
	     E1000_READ_REG(&adapter->hw, E1000_TDH(0))) &&
	    (E1000_READ_REG(&adapter->hw, E1000_TDFT) == 
	     E1000_READ_REG(&adapter->hw, E1000_TDFH)) &&
	    (E1000_READ_REG(&adapter->hw, E1000_TDFTS) ==
	     E1000_READ_REG(&adapter->hw, E1000_TDFHS)) &&
	    (E1000_READ_REG(&adapter->hw, E1000_TDFPC) == 0)) {
		/* Disable TX unit */
		tctl = E1000_READ_REG(&adapter->hw, E1000_TCTL);
		E1000_WRITE_REG(&adapter->hw, E1000_TCTL,
		    tctl & ~E1000_TCTL_EN);

		/* Reset FIFO pointers */
		E1000_WRITE_REG(&adapter->hw, E1000_TDFT,
		    adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, E1000_TDFH,
		    adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, E1000_TDFTS,
		    adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, E1000_TDFHS,
		    adapter->tx_head_addr);

		/* Re-enable TX unit */
		E1000_WRITE_REG(&adapter->hw, E1000_TCTL, tctl);
		E1000_WRITE_FLUSH(&adapter->hw);

		adapter->tx_fifo_head = 0;
		adapter->tx_fifo_reset_cnt++;

		return (TRUE);
	} else {
		return (FALSE);
	}
}

static void
em_set_promisc(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	uint32_t reg_rctl;

	reg_rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);

	if (ifp->if_flags & IFF_PROMISC) {
		reg_rctl |= (E1000_RCTL_UPE | E1000_RCTL_MPE);
		/* Turn this on if you want to see bad packets */
		if (em_debug_sbp)
			reg_rctl |= E1000_RCTL_SBP;
		E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
	} else if (ifp->if_flags & IFF_ALLMULTI) {
		reg_rctl |= E1000_RCTL_MPE;
		reg_rctl &= ~E1000_RCTL_UPE;
		E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
	}
}

static void
em_disable_promisc(struct adapter *adapter)
{
	uint32_t reg_rctl;

	reg_rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);

	reg_rctl &= ~E1000_RCTL_UPE;
	reg_rctl &= ~E1000_RCTL_MPE;
	reg_rctl &= ~E1000_RCTL_SBP;
	E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
}

static void
em_set_multi(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	struct ifmultiaddr *ifma;
	uint32_t reg_rctl = 0;
	uint8_t  mta[512]; /* Largest MTS is 4096 bits */
	int mcnt = 0;

	if (adapter->hw.mac.type == e1000_82542 && 
	    adapter->hw.revision_id == E1000_REVISION_2) {
		reg_rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);
		if (adapter->hw.bus.pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
			e1000_pci_clear_mwi(&adapter->hw);
		reg_rctl |= E1000_RCTL_RST;
		E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
		msec_delay(5);
	}

	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;

		if (mcnt == MAX_NUM_MULTICAST_ADDRESSES)
			break;

		bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
		    &mta[mcnt * ETHER_ADDR_LEN], ETHER_ADDR_LEN);
		mcnt++;
	}

	if (mcnt >= MAX_NUM_MULTICAST_ADDRESSES) {
		reg_rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);
		reg_rctl |= E1000_RCTL_MPE;
		E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
	} else {
		e1000_update_mc_addr_list(&adapter->hw, mta,
		    mcnt, 1, adapter->hw.mac.rar_entry_count);
	}

	if (adapter->hw.mac.type == e1000_82542 && 
	    adapter->hw.revision_id == E1000_REVISION_2) {
		reg_rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);
		reg_rctl &= ~E1000_RCTL_RST;
		E1000_WRITE_REG(&adapter->hw, E1000_RCTL, reg_rctl);
		msec_delay(5);
		if (adapter->hw.bus.pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
			e1000_pci_set_mwi(&adapter->hw);
	}
}

/*
 * This routine checks for link status and updates statistics.
 */
static void
em_timer(void *xsc)
{
	struct adapter *adapter = xsc;
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	em_update_link_status(adapter);
	em_update_stats(adapter);

	/* Reset LAA into RAR[0] on 82571 */
	if (e1000_get_laa_state_82571(&adapter->hw) == TRUE)
		e1000_rar_set(&adapter->hw, adapter->hw.mac.addr, 0);

	if (em_display_debug_stats && (ifp->if_flags & IFF_RUNNING))
		em_print_hw_stats(adapter);

	em_smartspeed(adapter);

	callout_reset(&adapter->timer, hz, em_timer, adapter);

	lwkt_serialize_exit(ifp->if_serializer);
}

static void
em_update_link_status(struct adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	device_t dev = adapter->dev;
	uint32_t link_check = 0;

	/* Get the cached link value or read phy for real */
	switch (hw->phy.media_type) {
	case e1000_media_type_copper:
		if (hw->mac.get_link_status) {
			/* Do the work to read phy */
			e1000_check_for_link(hw);
			link_check = !hw->mac.get_link_status;
			if (link_check) /* ESB2 fix */
				e1000_cfg_on_link_up(hw);
		} else {
			link_check = TRUE;
		}
		break;

	case e1000_media_type_fiber:
		e1000_check_for_link(hw);
		link_check =
			E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU;
		break;

	case e1000_media_type_internal_serdes:
		e1000_check_for_link(hw);
		link_check = adapter->hw.mac.serdes_has_link;
		break;

	case e1000_media_type_unknown:
	default:
		break;
	}

	/* Now check for a transition */
	if (link_check && adapter->link_active == 0) {
		e1000_get_speed_and_duplex(hw, &adapter->link_speed,
		    &adapter->link_duplex);

		/*
		 * Check if we should enable/disable SPEED_MODE bit on
		 * 82571/82572
		 */
		if (hw->mac.type == e1000_82571 ||
		    hw->mac.type == e1000_82572) {
			int tarc0;

			tarc0 = E1000_READ_REG(hw, E1000_TARC(0));
			if (adapter->link_speed != SPEED_1000)
				tarc0 &= ~SPEED_MODE_BIT;
			else
				tarc0 |= SPEED_MODE_BIT;
			E1000_WRITE_REG(hw, E1000_TARC(0), tarc0);
		}
		if (bootverbose) {
			device_printf(dev, "Link is up %d Mbps %s\n",
			    adapter->link_speed,
			    ((adapter->link_duplex == FULL_DUPLEX) ?
			    "Full Duplex" : "Half Duplex"));
		}
		adapter->link_active = 1;
		adapter->smartspeed = 0;
		ifp->if_baudrate = adapter->link_speed * 1000000;
		ifp->if_link_state = LINK_STATE_UP;
		if_link_state_change(ifp);
	} else if (!link_check && adapter->link_active == 1) {
		ifp->if_baudrate = adapter->link_speed = 0;
		adapter->link_duplex = 0;
		if (bootverbose)
			device_printf(dev, "Link is Down\n");
		adapter->link_active = 0;
#if 0
		/* Link down, disable watchdog */
		if->if_timer = 0;
#endif
		ifp->if_link_state = LINK_STATE_DOWN;
		if_link_state_change(ifp);
	}
}

static void
em_stop(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	int i;

	ASSERT_SERIALIZED(ifp->if_serializer);

	em_disable_intr(adapter);

	callout_stop(&adapter->timer);
	callout_stop(&adapter->tx_fifo_timer);

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;

	e1000_reset_hw(&adapter->hw);
	if (adapter->hw.mac.type >= e1000_82544)
		E1000_WRITE_REG(&adapter->hw, E1000_WUC, 0);

	for (i = 0; i < adapter->num_tx_desc; i++) {
		struct em_buffer *tx_buffer = &adapter->tx_buffer_area[i];

		if (tx_buffer->m_head != NULL) {
			bus_dmamap_unload(adapter->txtag, tx_buffer->map);
			m_freem(tx_buffer->m_head);
			tx_buffer->m_head = NULL;
		}
		tx_buffer->next_eop = -1;
	}

	for (i = 0; i < adapter->num_rx_desc; i++) {
		struct em_buffer *rx_buffer = &adapter->rx_buffer_area[i];

		if (rx_buffer->m_head != NULL) {
			bus_dmamap_unload(adapter->rxtag, rx_buffer->map);
			m_freem(rx_buffer->m_head);
			rx_buffer->m_head = NULL;
		}
	}

	if (adapter->fmp != NULL)
		m_freem(adapter->fmp);
	adapter->fmp = NULL;
	adapter->lmp = NULL;

	adapter->csum_flags = 0;
	adapter->csum_ehlen = 0;
	adapter->csum_iphlen = 0;
}

static int
em_get_hw_info(struct adapter *adapter)
{
	device_t dev = adapter->dev;

	/* Save off the information about this board */
	adapter->hw.vendor_id = pci_get_vendor(dev);
	adapter->hw.device_id = pci_get_device(dev);
	adapter->hw.revision_id = pci_get_revid(dev);
	adapter->hw.subsystem_vendor_id = pci_get_subvendor(dev);
	adapter->hw.subsystem_device_id = pci_get_subdevice(dev);

	/* Do Shared Code Init and Setup */
	if (e1000_set_mac_type(&adapter->hw))
		return ENXIO;
	return 0;
}

static int
em_alloc_pci_res(struct adapter *adapter)
{
	device_t dev = adapter->dev;
	int val, rid, error = E1000_SUCCESS;

	/* Enable bus mastering */
	pci_enable_busmaster(dev);

	adapter->memory_rid = EM_BAR_MEM;
	adapter->memory = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
				&adapter->memory_rid, RF_ACTIVE);
	if (adapter->memory == NULL) {
		device_printf(dev, "Unable to allocate bus resource: memory\n");
		return (ENXIO);
	}
	adapter->osdep.mem_bus_space_tag =
	    rman_get_bustag(adapter->memory);
	adapter->osdep.mem_bus_space_handle =
	    rman_get_bushandle(adapter->memory);

	/* XXX This is quite goofy, it is not actually used */
	adapter->hw.hw_addr = (uint8_t *)&adapter->osdep.mem_bus_space_handle;

	/* Only older adapters use IO mapping */
	if (adapter->hw.mac.type > e1000_82543 &&
	    adapter->hw.mac.type < e1000_82571) {
		/* Figure our where our IO BAR is ? */
		for (rid = PCIR_BAR(0); rid < PCIR_CARDBUSCIS;) {
			val = pci_read_config(dev, rid, 4);
			if (EM_BAR_TYPE(val) == EM_BAR_TYPE_IO) {
				adapter->io_rid = rid;
				break;
			}
			rid += 4;
			/* check for 64bit BAR */
			if (EM_BAR_MEM_TYPE(val) == EM_BAR_MEM_TYPE_64BIT)
				rid += 4;
		}
		if (rid >= PCIR_CARDBUSCIS) {
			device_printf(dev, "Unable to locate IO BAR\n");
			return (ENXIO);
		}
		adapter->ioport = bus_alloc_resource_any(dev, SYS_RES_IOPORT,
					&adapter->io_rid, RF_ACTIVE);
		if (adapter->ioport == NULL) {
			device_printf(dev, "Unable to allocate bus resource: "
			    "ioport\n");
			return (ENXIO);
		}
		adapter->hw.io_base = 0;
		adapter->osdep.io_bus_space_tag =
		    rman_get_bustag(adapter->ioport);
		adapter->osdep.io_bus_space_handle =
		    rman_get_bushandle(adapter->ioport);
	}

	adapter->intr_rid = 0;
	adapter->intr_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
				&adapter->intr_rid,
				RF_SHAREABLE | RF_ACTIVE);
	if (adapter->intr_res == NULL) {
		device_printf(dev, "Unable to allocate bus resource: "
		    "interrupt\n");
		return (ENXIO);
	}

	adapter->hw.bus.pci_cmd_word = pci_read_config(dev, PCIR_COMMAND, 2);
	adapter->hw.back = &adapter->osdep;
	return (error);
}

static void
em_free_pci_res(struct adapter *adapter)
{
	device_t dev = adapter->dev;

	if (adapter->intr_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ,
		    adapter->intr_rid, adapter->intr_res);
	}

	if (adapter->memory != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY,
		    adapter->memory_rid, adapter->memory);
	}

	if (adapter->flash != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY,
		    adapter->flash_rid, adapter->flash);
	}

	if (adapter->ioport != NULL) {
		bus_release_resource(dev, SYS_RES_IOPORT,
		    adapter->io_rid, adapter->ioport);
	}
}

static int
em_hw_init(struct adapter *adapter)
{
	device_t dev = adapter->dev;
	uint16_t rx_buffer_size;

	/* Issue a global reset */
	e1000_reset_hw(&adapter->hw);

	/* Get control from any management/hw control */
	if ((adapter->hw.mac.type == e1000_82573 ||
	     adapter->hw.mac.type == e1000_ich8lan ||
	     adapter->hw.mac.type == e1000_ich10lan ||
	     adapter->hw.mac.type == e1000_ich9lan) &&
	    e1000_check_mng_mode(&adapter->hw))
		em_get_hw_control(adapter);

	/* When hardware is reset, fifo_head is also reset */
	adapter->tx_fifo_head = 0;

	/* Set up smart power down as default off on newer adapters. */
	if (!em_smart_pwr_down &&
	    (adapter->hw.mac.type == e1000_82571 ||
	     adapter->hw.mac.type == e1000_82572)) {
		uint16_t phy_tmp = 0;

		/* Speed up time to link by disabling smart power down. */
		e1000_read_phy_reg(&adapter->hw,
		    IGP02E1000_PHY_POWER_MGMT, &phy_tmp);
		phy_tmp &= ~IGP02E1000_PM_SPD;
		e1000_write_phy_reg(&adapter->hw,
		    IGP02E1000_PHY_POWER_MGMT, phy_tmp);
	}

	/*
	 * These parameters control the automatic generation (Tx) and
	 * response (Rx) to Ethernet PAUSE frames.
	 * - High water mark should allow for at least two frames to be
	 *   received after sending an XOFF.
	 * - Low water mark works best when it is very near the high water mark.
	 *   This allows the receiver to restart by sending XON when it has
	 *   drained a bit. Here we use an arbitary value of 1500 which will
	 *   restart after one full frame is pulled from the buffer. There
	 *   could be several smaller frames in the buffer and if so they will
	 *   not trigger the XON until their total number reduces the buffer
	 *   by 1500.
	 * - The pause time is fairly large at 1000 x 512ns = 512 usec.
	 */
	rx_buffer_size =
		(E1000_READ_REG(&adapter->hw, E1000_PBA) & 0xffff) << 10;

	adapter->hw.fc.high_water = rx_buffer_size -
				    roundup2(adapter->max_frame_size, 1024);
	adapter->hw.fc.low_water = adapter->hw.fc.high_water - 1500;

	if (adapter->hw.mac.type == e1000_80003es2lan)
		adapter->hw.fc.pause_time = 0xFFFF;
	else
		adapter->hw.fc.pause_time = EM_FC_PAUSE_TIME;
	adapter->hw.fc.send_xon = TRUE;
	adapter->hw.fc.requested_mode = e1000_fc_full;

	if (e1000_init_hw(&adapter->hw) < 0) {
		device_printf(dev, "Hardware Initialization Failed\n");
		return (EIO);
	}

	e1000_check_for_link(&adapter->hw);

	return (0);
}

static void
em_setup_ifp(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	if_initname(ifp, device_get_name(adapter->dev),
		    device_get_unit(adapter->dev));
	ifp->if_softc = adapter;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init =  em_init;
	ifp->if_ioctl = em_ioctl;
	ifp->if_start = em_start;
#ifdef DEVICE_POLLING
	ifp->if_poll = em_poll;
#endif
	ifp->if_watchdog = em_watchdog;
	ifq_set_maxlen(&ifp->if_snd, adapter->num_tx_desc - 1);
	ifq_set_ready(&ifp->if_snd);

	ether_ifattach(ifp, adapter->hw.mac.addr, NULL);

	if (adapter->hw.mac.type >= e1000_82543)
		ifp->if_capabilities = IFCAP_HWCSUM;

	ifp->if_capabilities |= IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_MTU;
	ifp->if_capenable = ifp->if_capabilities;

	if (ifp->if_capenable & IFCAP_TXCSUM)
		ifp->if_hwassist = EM_CSUM_FEATURES;

	/*
	 * Tell the upper layer(s) we support long frames.
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);

	/*
	 * Specify the media types supported by this adapter and register
	 * callbacks to update media and link information
	 */
	ifmedia_init(&adapter->media, IFM_IMASK,
		     em_media_change, em_media_status);
	if (adapter->hw.phy.media_type == e1000_media_type_fiber ||
	    adapter->hw.phy.media_type == e1000_media_type_internal_serdes) {
		u_char fiber_type = IFM_1000_SX; /* default type */

		if (adapter->hw.mac.type == e1000_82545)
			fiber_type = IFM_1000_LX;
		ifmedia_add(&adapter->media, IFM_ETHER | fiber_type | IFM_FDX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | fiber_type, 0, NULL);
	} else {
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_10_T, 0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_10_T | IFM_FDX,
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_100_TX,
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_100_TX | IFM_FDX,
			    0, NULL);
		if (adapter->hw.phy.type != e1000_phy_ife) {
			ifmedia_add(&adapter->media,
				IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
			ifmedia_add(&adapter->media,
				IFM_ETHER | IFM_1000_T, 0, NULL);
		}
	}
	ifmedia_add(&adapter->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&adapter->media, IFM_ETHER | IFM_AUTO);
}


/*
 * Workaround for SmartSpeed on 82541 and 82547 controllers
 */
static void
em_smartspeed(struct adapter *adapter)
{
	uint16_t phy_tmp;

	if (adapter->link_active || adapter->hw.phy.type != e1000_phy_igp ||
	    adapter->hw.mac.autoneg == 0 ||
	    (adapter->hw.phy.autoneg_advertised & ADVERTISE_1000_FULL) == 0)
		return;

	if (adapter->smartspeed == 0) {
		/*
		 * If Master/Slave config fault is asserted twice,
		 * we assume back-to-back
		 */
		e1000_read_phy_reg(&adapter->hw, PHY_1000T_STATUS, &phy_tmp);
		if (!(phy_tmp & SR_1000T_MS_CONFIG_FAULT))
			return;
		e1000_read_phy_reg(&adapter->hw, PHY_1000T_STATUS, &phy_tmp);
		if (phy_tmp & SR_1000T_MS_CONFIG_FAULT) {
			e1000_read_phy_reg(&adapter->hw,
			    PHY_1000T_CTRL, &phy_tmp);
			if (phy_tmp & CR_1000T_MS_ENABLE) {
				phy_tmp &= ~CR_1000T_MS_ENABLE;
				e1000_write_phy_reg(&adapter->hw,
				    PHY_1000T_CTRL, phy_tmp);
				adapter->smartspeed++;
				if (adapter->hw.mac.autoneg &&
				    !e1000_phy_setup_autoneg(&adapter->hw) &&
				    !e1000_read_phy_reg(&adapter->hw,
				     PHY_CONTROL, &phy_tmp)) {
					phy_tmp |= MII_CR_AUTO_NEG_EN |
						   MII_CR_RESTART_AUTO_NEG;
					e1000_write_phy_reg(&adapter->hw,
					    PHY_CONTROL, phy_tmp);
				}
			}
		}
		return;
	} else if (adapter->smartspeed == EM_SMARTSPEED_DOWNSHIFT) {
		/* If still no link, perhaps using 2/3 pair cable */
		e1000_read_phy_reg(&adapter->hw, PHY_1000T_CTRL, &phy_tmp);
		phy_tmp |= CR_1000T_MS_ENABLE;
		e1000_write_phy_reg(&adapter->hw, PHY_1000T_CTRL, phy_tmp);
		if (adapter->hw.mac.autoneg &&
		    !e1000_phy_setup_autoneg(&adapter->hw) &&
		    !e1000_read_phy_reg(&adapter->hw, PHY_CONTROL, &phy_tmp)) {
			phy_tmp |= MII_CR_AUTO_NEG_EN | MII_CR_RESTART_AUTO_NEG;
			e1000_write_phy_reg(&adapter->hw, PHY_CONTROL, phy_tmp);
		}
	}

	/* Restart process after EM_SMARTSPEED_MAX iterations */
	if (adapter->smartspeed++ == EM_SMARTSPEED_MAX)
		adapter->smartspeed = 0;
}

static int
em_dma_malloc(struct adapter *adapter, bus_size_t size,
	      struct em_dma_alloc *dma)
{
	dma->dma_vaddr = bus_dmamem_coherent_any(adapter->parent_dtag,
				EM_DBA_ALIGN, size, BUS_DMA_WAITOK,
				&dma->dma_tag, &dma->dma_map,
				&dma->dma_paddr);
	if (dma->dma_vaddr == NULL)
		return ENOMEM;
	else
		return 0;
}

static void
em_dma_free(struct adapter *adapter, struct em_dma_alloc *dma)
{
	if (dma->dma_tag == NULL)
		return;
	bus_dmamap_unload(dma->dma_tag, dma->dma_map);
	bus_dmamem_free(dma->dma_tag, dma->dma_vaddr, dma->dma_map);
	bus_dma_tag_destroy(dma->dma_tag);
}

static int
em_create_tx_ring(struct adapter *adapter)
{
	device_t dev = adapter->dev;
	struct em_buffer *tx_buffer;
	int error, i;

	adapter->tx_buffer_area =
		kmalloc(sizeof(struct em_buffer) * adapter->num_tx_desc,
			M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tags for tx buffers
	 */
	error = bus_dma_tag_create(adapter->parent_dtag, /* parent */
			1, 0,			/* alignment, bounds */
			BUS_SPACE_MAXADDR,	/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			EM_TSO_SIZE,		/* maxsize */
			EM_MAX_SCATTER,		/* nsegments */
			EM_MAX_SEGSIZE,		/* maxsegsize */
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW |
			BUS_DMA_ONEBPAGE,	/* flags */
			&adapter->txtag);
	if (error) {
		device_printf(dev, "Unable to allocate TX DMA tag\n");
		kfree(adapter->tx_buffer_area, M_DEVBUF);
		adapter->tx_buffer_area = NULL;
		return error;
	}

	/*
	 * Create DMA maps for tx buffers
	 */
	for (i = 0; i < adapter->num_tx_desc; i++) {
		tx_buffer = &adapter->tx_buffer_area[i];

		error = bus_dmamap_create(adapter->txtag,
					  BUS_DMA_WAITOK | BUS_DMA_ONEBPAGE,
					  &tx_buffer->map);
		if (error) {
			device_printf(dev, "Unable to create TX DMA map\n");
			em_destroy_tx_ring(adapter, i);
			return error;
		}
		tx_buffer->next_eop = -1;
	}
	return (0);
}

static void
em_init_tx_ring(struct adapter *adapter)
{
	/* Clear the old ring contents */
	bzero(adapter->tx_desc_base,
	    (sizeof(struct e1000_tx_desc)) * adapter->num_tx_desc);

	/* Reset state */
	adapter->next_avail_tx_desc = 0;
	adapter->next_tx_to_clean = 0;
	adapter->num_tx_desc_avail = adapter->num_tx_desc;
}

static void
em_init_tx_unit(struct adapter *adapter)
{
	uint32_t tctl, tarc, tipg = 0;
	uint64_t bus_addr;

	/* Setup the Base and Length of the Tx Descriptor Ring */
	bus_addr = adapter->txdma.dma_paddr;
	E1000_WRITE_REG(&adapter->hw, E1000_TDLEN(0),
	    adapter->num_tx_desc * sizeof(struct e1000_tx_desc));
	E1000_WRITE_REG(&adapter->hw, E1000_TDBAH(0),
	    (uint32_t)(bus_addr >> 32));
	E1000_WRITE_REG(&adapter->hw, E1000_TDBAL(0),
	    (uint32_t)bus_addr);
	/* Setup the HW Tx Head and Tail descriptor pointers */
	E1000_WRITE_REG(&adapter->hw, E1000_TDT(0), 0);
	E1000_WRITE_REG(&adapter->hw, E1000_TDH(0), 0);

	/* Set the default values for the Tx Inter Packet Gap timer */
	switch (adapter->hw.mac.type) {
	case e1000_82542:
		tipg = DEFAULT_82542_TIPG_IPGT;
		tipg |= DEFAULT_82542_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
		tipg |= DEFAULT_82542_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
		break;

	case e1000_80003es2lan:
		tipg = DEFAULT_82543_TIPG_IPGR1;
		tipg |= DEFAULT_80003ES2LAN_TIPG_IPGR2 <<
		    E1000_TIPG_IPGR2_SHIFT;
		break;

	default:
		if (adapter->hw.phy.media_type == e1000_media_type_fiber ||
		    adapter->hw.phy.media_type ==
		    e1000_media_type_internal_serdes)
			tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
		else
			tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
		tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
		tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
		break;
	}

	E1000_WRITE_REG(&adapter->hw, E1000_TIPG, tipg);
	E1000_WRITE_REG(&adapter->hw, E1000_TIDV, adapter->tx_int_delay.value);
	if(adapter->hw.mac.type >= e1000_82540) {
		E1000_WRITE_REG(&adapter->hw, E1000_TADV,
		    adapter->tx_abs_int_delay.value);
	}

	if (adapter->hw.mac.type == e1000_82571 ||
	    adapter->hw.mac.type == e1000_82572) {
		tarc = E1000_READ_REG(&adapter->hw, E1000_TARC(0));
		tarc |= SPEED_MODE_BIT;
		E1000_WRITE_REG(&adapter->hw, E1000_TARC(0), tarc);
	} else if (adapter->hw.mac.type == e1000_80003es2lan) {
		tarc = E1000_READ_REG(&adapter->hw, E1000_TARC(0));
		tarc |= 1;
		E1000_WRITE_REG(&adapter->hw, E1000_TARC(0), tarc);
		tarc = E1000_READ_REG(&adapter->hw, E1000_TARC(1));
		tarc |= 1;
		E1000_WRITE_REG(&adapter->hw, E1000_TARC(1), tarc);
	}

	/* Program the Transmit Control Register */
	tctl = E1000_READ_REG(&adapter->hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN |
		(E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);

	if (adapter->hw.mac.type >= e1000_82571)
		tctl |= E1000_TCTL_MULR;

	/* This write will effectively turn on the transmit unit. */
	E1000_WRITE_REG(&adapter->hw, E1000_TCTL, tctl);

	/* Setup Transmit Descriptor Base Settings */   
	adapter->txd_cmd = E1000_TXD_CMD_IFCS;

	if (adapter->tx_int_delay.value > 0)
		adapter->txd_cmd |= E1000_TXD_CMD_IDE;
}

static void
em_destroy_tx_ring(struct adapter *adapter, int ndesc)
{
	struct em_buffer *tx_buffer;
	int i;

	if (adapter->tx_buffer_area == NULL)
		return;

	for (i = 0; i < ndesc; i++) {
		tx_buffer = &adapter->tx_buffer_area[i];

		KKASSERT(tx_buffer->m_head == NULL);
		bus_dmamap_destroy(adapter->txtag, tx_buffer->map);
	}
	bus_dma_tag_destroy(adapter->txtag);

	kfree(adapter->tx_buffer_area, M_DEVBUF);
	adapter->tx_buffer_area = NULL;
}

/*
 * The offload context needs to be set when we transfer the first
 * packet of a particular protocol (TCP/UDP).  This routine has been
 * enhanced to deal with inserted VLAN headers.
 *
 * If the new packet's ether header length, ip header length and
 * csum offloading type are same as the previous packet, we should
 * avoid allocating a new csum context descriptor; mainly to take
 * advantage of the pipeline effect of the TX data read request.
 */
static void
em_txcsum(struct adapter *adapter, struct mbuf *mp,
	  uint32_t *txd_upper, uint32_t *txd_lower)
{
	struct e1000_context_desc *TXD;
	struct em_buffer *tx_buffer;
	struct ether_vlan_header *eh;
	struct ip *ip;
	int curr_txd, ehdrlen, csum_flags;
	uint32_t cmd, hdr_len, ip_hlen;
	uint16_t etype;

	/*
	 * Determine where frame payload starts.
	 * Jump over vlan headers if already present,
	 * helpful for QinQ too.
	 */
	KASSERT(mp->m_len >= sizeof(struct ether_vlan_header),
		("em_txcsum_pullup is not called?\n"));
	eh = mtod(mp, struct ether_vlan_header *);
	if (eh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		etype = ntohs(eh->evl_proto);
		ehdrlen = ETHER_HDR_LEN + EVL_ENCAPLEN;
	} else {
		etype = ntohs(eh->evl_encap_proto);
		ehdrlen = ETHER_HDR_LEN;
	}

	/*
	 * We only support TCP/UDP for IPv4 for the moment.
	 * TODO: Support SCTP too when it hits the tree.
	 */
	if (etype != ETHERTYPE_IP)
		return;

	KASSERT(mp->m_len >= ehdrlen + EM_IPVHL_SIZE,
		("em_txcsum_pullup is not called?\n"));

	/* NOTE: We could only safely access ip.ip_vhl part */
	ip = (struct ip *)(mp->m_data + ehdrlen);
	ip_hlen = ip->ip_hl << 2;

	csum_flags = mp->m_pkthdr.csum_flags & EM_CSUM_FEATURES;

	if (adapter->csum_ehlen == ehdrlen &&
	    adapter->csum_iphlen == ip_hlen &&
	    adapter->csum_flags == csum_flags) {
		/*
		 * Same csum offload context as the previous packets;
		 * just return.
		 */
		*txd_upper = adapter->csum_txd_upper;
		*txd_lower = adapter->csum_txd_lower;
		return;
	}

	/*
	 * Setup a new csum offload context.
	 */

	curr_txd = adapter->next_avail_tx_desc;
	tx_buffer = &adapter->tx_buffer_area[curr_txd];
	TXD = (struct e1000_context_desc *)&adapter->tx_desc_base[curr_txd];

	cmd = 0;

	/* Setup of IP header checksum. */
	if (csum_flags & CSUM_IP) {
		/*
		 * Start offset for header checksum calculation.
		 * End offset for header checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->lower_setup.ip_fields.ipcss = ehdrlen;
		TXD->lower_setup.ip_fields.ipcse =
		    htole16(ehdrlen + ip_hlen - 1);
		TXD->lower_setup.ip_fields.ipcso =
		    ehdrlen + offsetof(struct ip, ip_sum);
		cmd |= E1000_TXD_CMD_IP;
		*txd_upper |= E1000_TXD_POPTS_IXSM << 8;
	}
	hdr_len = ehdrlen + ip_hlen;

	if (csum_flags & CSUM_TCP) {
		/*
		 * Start offset for payload checksum calculation.
		 * End offset for payload checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->upper_setup.tcp_fields.tucss = hdr_len;
		TXD->upper_setup.tcp_fields.tucse = htole16(0);
		TXD->upper_setup.tcp_fields.tucso =
		    hdr_len + offsetof(struct tcphdr, th_sum);
		cmd |= E1000_TXD_CMD_TCP;
		*txd_upper |= E1000_TXD_POPTS_TXSM << 8;
	} else if (csum_flags & CSUM_UDP) {
		/*
		 * Start offset for header checksum calculation.
		 * End offset for header checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->upper_setup.tcp_fields.tucss = hdr_len;
		TXD->upper_setup.tcp_fields.tucse = htole16(0);
		TXD->upper_setup.tcp_fields.tucso =
		    hdr_len + offsetof(struct udphdr, uh_sum);
		*txd_upper |= E1000_TXD_POPTS_TXSM << 8;
	}

	*txd_lower = E1000_TXD_CMD_DEXT |	/* Extended descr type */
		     E1000_TXD_DTYP_D;		/* Data descr */

	/* Save the information for this csum offloading context */
	adapter->csum_ehlen = ehdrlen;
	adapter->csum_iphlen = ip_hlen;
	adapter->csum_flags = csum_flags;
	adapter->csum_txd_upper = *txd_upper;
	adapter->csum_txd_lower = *txd_lower;

	TXD->tcp_seg_setup.data = htole32(0);
	TXD->cmd_and_length =
	    htole32(adapter->txd_cmd | E1000_TXD_CMD_DEXT | cmd);
	tx_buffer->m_head = NULL;
	tx_buffer->next_eop = -1;

	if (++curr_txd == adapter->num_tx_desc)
		curr_txd = 0;

	KKASSERT(adapter->num_tx_desc_avail > 0);
	adapter->num_tx_desc_avail--;

	adapter->next_avail_tx_desc = curr_txd;
}

static int
em_txcsum_pullup(struct adapter *adapter, struct mbuf **m0)
{
	struct mbuf *m = *m0;
	struct ether_header *eh;
	int len;

	adapter->tx_csum_try_pullup++;

	len = ETHER_HDR_LEN + EM_IPVHL_SIZE;

	if (__predict_false(!M_WRITABLE(m))) {
		if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
			adapter->tx_csum_drop1++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		eh = mtod(m, struct ether_header *);

		if (eh->ether_type == htons(ETHERTYPE_VLAN))
			len += EVL_ENCAPLEN;

		if (__predict_false(m->m_len < len)) {
			adapter->tx_csum_drop2++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		return 0;
	}

	if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
		adapter->tx_csum_pullup1++;
		m = m_pullup(m, ETHER_HDR_LEN);
		if (m == NULL) {
			adapter->tx_csum_pullup1_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	eh = mtod(m, struct ether_header *);

	if (eh->ether_type == htons(ETHERTYPE_VLAN))
		len += EVL_ENCAPLEN;

	if (__predict_false(m->m_len < len)) {
		adapter->tx_csum_pullup2++;
		m = m_pullup(m, len);
		if (m == NULL) {
			adapter->tx_csum_pullup2_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	return 0;
}

static void
em_txeof(struct adapter *adapter)
{
	int first, last, done, num_avail;
	struct em_buffer *tx_buffer;
	struct e1000_tx_desc *tx_desc, *eop_desc;
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	if (adapter->num_tx_desc_avail == adapter->num_tx_desc)
		return;

	num_avail = adapter->num_tx_desc_avail;
	first = adapter->next_tx_to_clean;

	tx_desc = &adapter->tx_desc_base[first];
	tx_buffer = &adapter->tx_buffer_area[first];
	last = tx_buffer->next_eop;
	eop_desc = &adapter->tx_desc_base[last];

	/*
	 * What this does is get the index of the
	 * first descriptor AFTER the EOP of the
	 * first packet, that way we can do the
	 * simple comparison on the inner while loop.
	 */
	if (++last == adapter->num_tx_desc)
		last = 0;
	done = last;

        while (eop_desc->upper.fields.status & E1000_TXD_STAT_DD) {
		/* We clean the range of the packet */
		while (first != done) {
			logif(pkt_txclean);

			tx_desc->upper.data = 0;
			tx_desc->lower.data = 0;
			tx_desc->buffer_addr = 0;
			num_avail++;

			if (tx_buffer->m_head) {
				ifp->if_opackets++;
				bus_dmamap_unload(adapter->txtag,
						  tx_buffer->map);
				m_freem(tx_buffer->m_head);
				tx_buffer->m_head = NULL;
			}
			tx_buffer->next_eop = -1;

			if (++first == adapter->num_tx_desc)
				first = 0;

			tx_buffer = &adapter->tx_buffer_area[first];
			tx_desc = &adapter->tx_desc_base[first];
		}

		/* See if we can continue to the next packet */
		last = tx_buffer->next_eop;
		if (last != -1) {
			eop_desc = &adapter->tx_desc_base[last];

			/* Get new done point */
			if (++last == adapter->num_tx_desc)
				last = 0;
			done = last;
		} else {
			break;
		}
	}
        adapter->next_tx_to_clean = first;
	adapter->num_tx_desc_avail = num_avail;

	if (adapter->num_tx_desc_avail > EM_TX_CLEANUP_THRESHOLD) {
		ifp->if_flags &= ~IFF_OACTIVE;

		/* All clean, turn off the timer */
		if (adapter->num_tx_desc_avail == adapter->num_tx_desc)
			ifp->if_timer = 0;
	}
}

/*
 * When Link is lost sometimes there is work still in the TX ring
 * which will result in a watchdog, rather than allow that do an
 * attempted cleanup and then reinit here.  Note that this has been
 * seens mostly with fiber adapters.
 */
static void
em_tx_purge(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	if (!adapter->link_active && ifp->if_timer) {
		em_txeof(adapter);
		if (ifp->if_timer) {
			if_printf(ifp, "Link lost, TX pending, reinit\n");
			ifp->if_timer = 0;
			em_init(adapter);
		}
	}
}

static int
em_newbuf(struct adapter *adapter, int i, int init)
{
	struct mbuf *m;
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	struct em_buffer *rx_buffer;
	int error, nseg;

	m = m_getcl(init ? MB_WAIT : MB_DONTWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		adapter->mbuf_cluster_failed++;
		if (init) {
			if_printf(&adapter->arpcom.ac_if,
				  "Unable to allocate RX mbuf\n");
		}
		return (ENOBUFS);
	}
	m->m_len = m->m_pkthdr.len = MCLBYTES;

	if (adapter->max_frame_size <= MCLBYTES - ETHER_ALIGN)
		m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf_segment(adapter->rxtag,
			adapter->rx_sparemap, m,
			&seg, 1, &nseg, BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		if (init) {
			if_printf(&adapter->arpcom.ac_if,
				  "Unable to load RX mbuf\n");
		}
		return (error);
	}

	rx_buffer = &adapter->rx_buffer_area[i];
	if (rx_buffer->m_head != NULL)
		bus_dmamap_unload(adapter->rxtag, rx_buffer->map);

	map = rx_buffer->map;
	rx_buffer->map = adapter->rx_sparemap;
	adapter->rx_sparemap = map;

	rx_buffer->m_head = m;

	adapter->rx_desc_base[i].buffer_addr = htole64(seg.ds_addr);
	return (0);
}

static int
em_create_rx_ring(struct adapter *adapter)
{
	device_t dev = adapter->dev;
	struct em_buffer *rx_buffer;
	int i, error;

	adapter->rx_buffer_area =
		kmalloc(sizeof(struct em_buffer) * adapter->num_rx_desc,
			M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tag for rx buffers
	 */
	error = bus_dma_tag_create(adapter->parent_dtag, /* parent */
			1, 0,			/* alignment, bounds */
			BUS_SPACE_MAXADDR,	/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			MCLBYTES,		/* maxsize */
			1,			/* nsegments */
			MCLBYTES,		/* maxsegsize */
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, /* flags */
			&adapter->rxtag);
	if (error) {
		device_printf(dev, "Unable to allocate RX DMA tag\n");
		kfree(adapter->rx_buffer_area, M_DEVBUF);
		adapter->rx_buffer_area = NULL;
		return error;
	}

	/*
	 * Create spare DMA map for rx buffers
	 */
	error = bus_dmamap_create(adapter->rxtag, BUS_DMA_WAITOK,
				  &adapter->rx_sparemap);
	if (error) {
		device_printf(dev, "Unable to create spare RX DMA map\n");
		bus_dma_tag_destroy(adapter->rxtag);
		kfree(adapter->rx_buffer_area, M_DEVBUF);
		adapter->rx_buffer_area = NULL;
		return error;
	}

	/*
	 * Create DMA maps for rx buffers
	 */
	for (i = 0; i < adapter->num_rx_desc; i++) {
		rx_buffer = &adapter->rx_buffer_area[i];

		error = bus_dmamap_create(adapter->rxtag, BUS_DMA_WAITOK,
					  &rx_buffer->map);
		if (error) {
			device_printf(dev, "Unable to create RX DMA map\n");
			em_destroy_rx_ring(adapter, i);
			return error;
		}
	}
	return (0);
}

static int
em_init_rx_ring(struct adapter *adapter)
{
	int i, error;

	/* Reset descriptor ring */
	bzero(adapter->rx_desc_base,
	    (sizeof(struct e1000_rx_desc)) * adapter->num_rx_desc);

	/* Allocate new ones. */
	for (i = 0; i < adapter->num_rx_desc; i++) {
		error = em_newbuf(adapter, i, 1);
		if (error)
			return (error);
	}

	/* Setup our descriptor pointers */
	adapter->next_rx_desc_to_check = 0;

	return (0);
}

static void
em_init_rx_unit(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	uint64_t bus_addr;
	uint32_t rctl, rxcsum;

	/*
	 * Make sure receives are disabled while setting
	 * up the descriptor ring
	 */
	rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);
	E1000_WRITE_REG(&adapter->hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

	if (adapter->hw.mac.type >= e1000_82540) {
		E1000_WRITE_REG(&adapter->hw, E1000_RADV,
		    adapter->rx_abs_int_delay.value);

		/*
		 * Set the interrupt throttling rate. Value is calculated
		 * as ITR = 1 / (INT_THROTTLE_CEIL * 256ns)
		 */
		if (adapter->int_throttle_ceil) {
			E1000_WRITE_REG(&adapter->hw, E1000_ITR,
				1000000000 / 256 / adapter->int_throttle_ceil);
		} else {
			E1000_WRITE_REG(&adapter->hw, E1000_ITR, 0);
		}
	}

	/* Disable accelerated ackknowledge */
	if (adapter->hw.mac.type == e1000_82574) {
		E1000_WRITE_REG(&adapter->hw,
		    E1000_RFCTL, E1000_RFCTL_ACK_DIS);
	}

	/* Setup the Base and Length of the Rx Descriptor Ring */
	bus_addr = adapter->rxdma.dma_paddr;
	E1000_WRITE_REG(&adapter->hw, E1000_RDLEN(0),
	    adapter->num_rx_desc * sizeof(struct e1000_rx_desc));
	E1000_WRITE_REG(&adapter->hw, E1000_RDBAH(0),
	    (uint32_t)(bus_addr >> 32));
	E1000_WRITE_REG(&adapter->hw, E1000_RDBAL(0),
	    (uint32_t)bus_addr);

	/* Setup the Receive Control Register */
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO |
		E1000_RCTL_RDMTS_HALF |
		(adapter->hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);

	/* Make sure VLAN Filters are off */
	rctl &= ~E1000_RCTL_VFE;

	if (e1000_tbi_sbp_enabled_82543(&adapter->hw))
		rctl |= E1000_RCTL_SBP;
	else
		rctl &= ~E1000_RCTL_SBP;

	switch (adapter->rx_buffer_len) {
	default:
	case 2048:
		rctl |= E1000_RCTL_SZ_2048;
		break;

	case 4096:
		rctl |= E1000_RCTL_SZ_4096 |
		    E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;

	case 8192:
		rctl |= E1000_RCTL_SZ_8192 |
		    E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;

	case 16384:
		rctl |= E1000_RCTL_SZ_16384 |
		    E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;
	}

	if (ifp->if_mtu > ETHERMTU)
		rctl |= E1000_RCTL_LPE;
	else
		rctl &= ~E1000_RCTL_LPE;

	/* Receive Checksum Offload for TCP and UDP */
	if (ifp->if_capenable & IFCAP_RXCSUM) {
		rxcsum = E1000_READ_REG(&adapter->hw, E1000_RXCSUM);
		rxcsum |= (E1000_RXCSUM_IPOFL | E1000_RXCSUM_TUOFL);
		E1000_WRITE_REG(&adapter->hw, E1000_RXCSUM, rxcsum);
	}

	/*
	 * XXX TEMPORARY WORKAROUND: on some systems with 82573
	 * long latencies are observed, like Lenovo X60. This
	 * change eliminates the problem, but since having positive
	 * values in RDTR is a known source of problems on other
	 * platforms another solution is being sought.
	 */
	if (adapter->hw.mac.type == e1000_82573)
		E1000_WRITE_REG(&adapter->hw, E1000_RDTR, 0x20);

	/* Enable Receives */
	E1000_WRITE_REG(&adapter->hw, E1000_RCTL, rctl);

	/*
	 * Setup the HW Rx Head and Tail Descriptor Pointers
	 */
	E1000_WRITE_REG(&adapter->hw, E1000_RDH(0), 0);
	E1000_WRITE_REG(&adapter->hw, E1000_RDT(0), adapter->num_rx_desc - 1);
}

static void
em_destroy_rx_ring(struct adapter *adapter, int ndesc)
{
	struct em_buffer *rx_buffer;
	int i;

	if (adapter->rx_buffer_area == NULL)
		return;

	for (i = 0; i < ndesc; i++) {
		rx_buffer = &adapter->rx_buffer_area[i];

		KKASSERT(rx_buffer->m_head == NULL);
		bus_dmamap_destroy(adapter->rxtag, rx_buffer->map);
	}
	bus_dmamap_destroy(adapter->rxtag, adapter->rx_sparemap);
	bus_dma_tag_destroy(adapter->rxtag);

	kfree(adapter->rx_buffer_area, M_DEVBUF);
	adapter->rx_buffer_area = NULL;
}

static void
em_rxeof(struct adapter *adapter, int count)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	uint8_t status, accept_frame = 0, eop = 0;
	uint16_t len, desc_len, prev_len_adj;
	struct e1000_rx_desc *current_desc;
	struct mbuf *mp;
	int i;
	struct mbuf_chain chain[MAXCPU];

	i = adapter->next_rx_desc_to_check;
	current_desc = &adapter->rx_desc_base[i];

	if (!(current_desc->status & E1000_RXD_STAT_DD))
		return;

	ether_input_chain_init(chain);

	while ((current_desc->status & E1000_RXD_STAT_DD) && count != 0) {
		struct mbuf *m = NULL;

		logif(pkt_receive);

		mp = adapter->rx_buffer_area[i].m_head;

		/*
		 * Can't defer bus_dmamap_sync(9) because TBI_ACCEPT
		 * needs to access the last received byte in the mbuf.
		 */
		bus_dmamap_sync(adapter->rxtag, adapter->rx_buffer_area[i].map,
				BUS_DMASYNC_POSTREAD);

		accept_frame = 1;
		prev_len_adj = 0;
		desc_len = le16toh(current_desc->length);
		status = current_desc->status;
		if (status & E1000_RXD_STAT_EOP) {
			count--;
			eop = 1;
			if (desc_len < ETHER_CRC_LEN) {
				len = 0;
				prev_len_adj = ETHER_CRC_LEN - desc_len;
			} else {
				len = desc_len - ETHER_CRC_LEN;
			}
		} else {
			eop = 0;
			len = desc_len;
		}

		if (current_desc->errors & E1000_RXD_ERR_FRAME_ERR_MASK) {
			uint8_t	last_byte;
			uint32_t pkt_len = desc_len;

			if (adapter->fmp != NULL)
				pkt_len += adapter->fmp->m_pkthdr.len;

			last_byte = *(mtod(mp, caddr_t) + desc_len - 1);
			if (TBI_ACCEPT(&adapter->hw, status,
			    current_desc->errors, pkt_len, last_byte,
			    adapter->min_frame_size, adapter->max_frame_size)) {
				e1000_tbi_adjust_stats_82543(&adapter->hw,
				    &adapter->stats, pkt_len,
				    adapter->hw.mac.addr,
				    adapter->max_frame_size);
				if (len > 0)
					len--;
			} else {
				accept_frame = 0;
			}
		}

		if (accept_frame) {
			if (em_newbuf(adapter, i, 0) != 0) {
				ifp->if_iqdrops++;
				goto discard;
			}

			/* Assign correct length to the current fragment */
			mp->m_len = len;

			if (adapter->fmp == NULL) {
				mp->m_pkthdr.len = len;
				adapter->fmp = mp; /* Store the first mbuf */
				adapter->lmp = mp;
			} else {
				/*
				 * Chain mbuf's together
				 */

				/*
				 * Adjust length of previous mbuf in chain if
				 * we received less than 4 bytes in the last
				 * descriptor.
				 */
				if (prev_len_adj > 0) {
					adapter->lmp->m_len -= prev_len_adj;
					adapter->fmp->m_pkthdr.len -=
					    prev_len_adj;
				}
				adapter->lmp->m_next = mp;
				adapter->lmp = adapter->lmp->m_next;
				adapter->fmp->m_pkthdr.len += len;
			}

			if (eop) {
				adapter->fmp->m_pkthdr.rcvif = ifp;
				ifp->if_ipackets++;

				if (ifp->if_capenable & IFCAP_RXCSUM) {
					em_rxcsum(adapter, current_desc,
						  adapter->fmp);
				}

				if (status & E1000_RXD_STAT_VP) {
					adapter->fmp->m_pkthdr.ether_vlantag =
					    (le16toh(current_desc->special) &
					    E1000_RXD_SPC_VLAN_MASK);
					adapter->fmp->m_flags |= M_VLANTAG;
				}
				m = adapter->fmp;
				adapter->fmp = NULL;
				adapter->lmp = NULL;
			}
		} else {
			ifp->if_ierrors++;
discard:
#ifdef foo
			/* Reuse loaded DMA map and just update mbuf chain */
			mp = adapter->rx_buffer_area[i].m_head;
			mp->m_len = mp->m_pkthdr.len = MCLBYTES;
			mp->m_data = mp->m_ext.ext_buf;
			mp->m_next = NULL;
			if (adapter->max_frame_size <= (MCLBYTES - ETHER_ALIGN))
				m_adj(mp, ETHER_ALIGN);
#endif
			if (adapter->fmp != NULL) {
				m_freem(adapter->fmp);
				adapter->fmp = NULL;
				adapter->lmp = NULL;
			}
			m = NULL;
		}

		/* Zero out the receive descriptors status. */
		current_desc->status = 0;

		if (m != NULL)
			ether_input_chain(ifp, m, chain);

		/* Advance our pointers to the next descriptor. */
		if (++i == adapter->num_rx_desc)
			i = 0;
		current_desc = &adapter->rx_desc_base[i];
	}
	adapter->next_rx_desc_to_check = i;

	ether_input_dispatch(chain);

	/* Advance the E1000's Receive Queue #0  "Tail Pointer". */
	if (--i < 0)
		i = adapter->num_rx_desc - 1;
	E1000_WRITE_REG(&adapter->hw, E1000_RDT(0), i);
}

static void
em_rxcsum(struct adapter *adapter, struct e1000_rx_desc *rx_desc,
	  struct mbuf *mp)
{
	/* 82543 or newer only */
	if (adapter->hw.mac.type < e1000_82543 ||
	    /* Ignore Checksum bit is set */
	    (rx_desc->status & E1000_RXD_STAT_IXSM))
		return;

	if ((rx_desc->status & E1000_RXD_STAT_IPCS) &&
	    !(rx_desc->errors & E1000_RXD_ERR_IPE)) {
		/* IP Checksum Good */
		mp->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;
	}

	if ((rx_desc->status & E1000_RXD_STAT_TCPCS) &&
	    !(rx_desc->errors & E1000_RXD_ERR_TCPE)) {
		mp->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
					   CSUM_PSEUDO_HDR |
					   CSUM_FRAG_NOT_CHECKED;
		mp->m_pkthdr.csum_data = htons(0xffff);
	}
}

static void
em_enable_intr(struct adapter *adapter)
{
	lwkt_serialize_handler_enable(adapter->arpcom.ac_if.if_serializer);
	E1000_WRITE_REG(&adapter->hw, E1000_IMS, IMS_ENABLE_MASK);
}

static void
em_disable_intr(struct adapter *adapter)
{
	uint32_t clear = 0xffffffff;

	/*
	 * The first version of 82542 had an errata where when link was forced
	 * it would stay up even up even if the cable was disconnected.
	 * Sequence errors were used to detect the disconnect and then the
	 * driver would unforce the link.  This code in the in the ISR.  For
	 * this to work correctly the Sequence error interrupt had to be
	 * enabled all the time.
	 */
	if (adapter->hw.mac.type == e1000_82542 &&
	    adapter->hw.revision_id == E1000_REVISION_2)
		clear &= ~E1000_IMC_RXSEQ;

	E1000_WRITE_REG(&adapter->hw, E1000_IMC, clear);

	lwkt_serialize_handler_disable(adapter->arpcom.ac_if.if_serializer);
}

/*
 * Bit of a misnomer, what this really means is
 * to enable OS management of the system... aka
 * to disable special hardware management features 
 */
static void
em_get_mgmt(struct adapter *adapter)
{
	/* A shared code workaround */
#define E1000_82542_MANC2H E1000_MANC2H
	if (adapter->has_manage) {
		int manc2h = E1000_READ_REG(&adapter->hw, E1000_MANC2H);
		int manc = E1000_READ_REG(&adapter->hw, E1000_MANC);

		/* disable hardware interception of ARP */
		manc &= ~(E1000_MANC_ARP_EN);

                /* enable receiving management packets to the host */
                if (adapter->hw.mac.type >= e1000_82571) {
			manc |= E1000_MANC_EN_MNG2HOST;
#define E1000_MNG2HOST_PORT_623 (1 << 5)
#define E1000_MNG2HOST_PORT_664 (1 << 6)
			manc2h |= E1000_MNG2HOST_PORT_623;
			manc2h |= E1000_MNG2HOST_PORT_664;
			E1000_WRITE_REG(&adapter->hw, E1000_MANC2H, manc2h);
		}

		E1000_WRITE_REG(&adapter->hw, E1000_MANC, manc);
	}
}

/*
 * Give control back to hardware management
 * controller if there is one.
 */
static void
em_rel_mgmt(struct adapter *adapter)
{
	if (adapter->has_manage) {
		int manc = E1000_READ_REG(&adapter->hw, E1000_MANC);

		/* re-enable hardware interception of ARP */
		manc |= E1000_MANC_ARP_EN;

		if (adapter->hw.mac.type >= e1000_82571)
			manc &= ~E1000_MANC_EN_MNG2HOST;

		E1000_WRITE_REG(&adapter->hw, E1000_MANC, manc);
	}
}

/*
 * em_get_hw_control() sets {CTRL_EXT|FWSM}:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that
 * the driver is loaded.  For AMT version (only with 82573)
 * of the f/w this means that the network i/f is open.
 */
static void
em_get_hw_control(struct adapter *adapter)
{
	uint32_t ctrl_ext, swsm;

	/* Let firmware know the driver has taken over */
	switch (adapter->hw.mac.type) {
	case e1000_82573:
		swsm = E1000_READ_REG(&adapter->hw, E1000_SWSM);
		E1000_WRITE_REG(&adapter->hw, E1000_SWSM,
		    swsm | E1000_SWSM_DRV_LOAD);
		break;
	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
	case e1000_ich8lan:
	case e1000_ich9lan:
	case e1000_ich10lan:
		ctrl_ext = E1000_READ_REG(&adapter->hw, E1000_CTRL_EXT);
		E1000_WRITE_REG(&adapter->hw, E1000_CTRL_EXT,
		    ctrl_ext | E1000_CTRL_EXT_DRV_LOAD);
		break;
	default:
		break;
	}
}

/*
 * em_rel_hw_control() resets {CTRL_EXT|FWSM}:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that the
 * driver is no longer loaded.  For AMT version (only with 82573)
 * of the f/w this means that the network i/f is closed.
 */
static void
em_rel_hw_control(struct adapter *adapter)
{
	uint32_t ctrl_ext, swsm;

	/* Let firmware taken over control of h/w */
	switch (adapter->hw.mac.type) {
	case e1000_82573:
		swsm = E1000_READ_REG(&adapter->hw, E1000_SWSM);
		E1000_WRITE_REG(&adapter->hw, E1000_SWSM,
		    swsm & ~E1000_SWSM_DRV_LOAD);
		break;

	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
	case e1000_ich8lan:
	case e1000_ich9lan:
	case e1000_ich10lan:
		ctrl_ext = E1000_READ_REG(&adapter->hw, E1000_CTRL_EXT);
		E1000_WRITE_REG(&adapter->hw, E1000_CTRL_EXT,
		    ctrl_ext & ~E1000_CTRL_EXT_DRV_LOAD);
		break;

	default:
		break;
	}
}

static int
em_is_valid_eaddr(const uint8_t *addr)
{
	char zero_addr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

	if ((addr[0] & 1) || !bcmp(addr, zero_addr, ETHER_ADDR_LEN))
		return (FALSE);

	return (TRUE);
}

/*
 * Enable PCI Wake On Lan capability
 */
void
em_enable_wol(device_t dev)
{
	uint16_t cap, status;
	uint8_t id;

	/* First find the capabilities pointer*/
	cap = pci_read_config(dev, PCIR_CAP_PTR, 2);

	/* Read the PM Capabilities */
	id = pci_read_config(dev, cap, 1);
	if (id != PCIY_PMG)     /* Something wrong */
		return;

	/*
	 * OK, we have the power capabilities,
	 * so now get the status register
	 */
	cap += PCIR_POWER_STATUS;
	status = pci_read_config(dev, cap, 2);
	status |= PCIM_PSTAT_PME | PCIM_PSTAT_PMEENABLE;
	pci_write_config(dev, cap, status, 2);
}


/*
 * 82544 Coexistence issue workaround.
 *    There are 2 issues.
 *       1. Transmit Hang issue.
 *    To detect this issue, following equation can be used...
 *	  SIZE[3:0] + ADDR[2:0] = SUM[3:0].
 *	  If SUM[3:0] is in between 1 to 4, we will have this issue.
 *
 *       2. DAC issue.
 *    To detect this issue, following equation can be used...
 *	  SIZE[3:0] + ADDR[2:0] = SUM[3:0].
 *	  If SUM[3:0] is in between 9 to c, we will have this issue.
 *
 *    WORKAROUND:
 *	  Make sure we do not have ending address
 *	  as 1,2,3,4(Hang) or 9,a,b,c (DAC)
 */
static uint32_t
em_82544_fill_desc(bus_addr_t address, uint32_t length, PDESC_ARRAY desc_array)
{
	uint32_t safe_terminator;

	/*
	 * Since issue is sensitive to length and address.
	 * Let us first check the address...
	 */
	if (length <= 4) {
		desc_array->descriptor[0].address = address;
		desc_array->descriptor[0].length = length;
		desc_array->elements = 1;
		return (desc_array->elements);
	}

	safe_terminator =
	(uint32_t)((((uint32_t)address & 0x7) + (length & 0xF)) & 0xF);

	/* If it does not fall between 0x1 to 0x4 and 0x9 to 0xC then return */
	if (safe_terminator == 0 ||
	    (safe_terminator > 4 && safe_terminator < 9) ||
	    (safe_terminator > 0xC && safe_terminator <= 0xF)) {
		desc_array->descriptor[0].address = address;
		desc_array->descriptor[0].length = length;
		desc_array->elements = 1;
		return (desc_array->elements);
	}

	desc_array->descriptor[0].address = address;
	desc_array->descriptor[0].length = length - 4;
	desc_array->descriptor[1].address = address + (length - 4);
	desc_array->descriptor[1].length = 4;
	desc_array->elements = 2;
	return (desc_array->elements);
}

static void
em_update_stats(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->arpcom.ac_if;

	if (adapter->hw.phy.media_type == e1000_media_type_copper ||
	    (E1000_READ_REG(&adapter->hw, E1000_STATUS) & E1000_STATUS_LU)) {
		adapter->stats.symerrs +=
			E1000_READ_REG(&adapter->hw, E1000_SYMERRS);
		adapter->stats.sec += E1000_READ_REG(&adapter->hw, E1000_SEC);
	}
	adapter->stats.crcerrs += E1000_READ_REG(&adapter->hw, E1000_CRCERRS);
	adapter->stats.mpc += E1000_READ_REG(&adapter->hw, E1000_MPC);
	adapter->stats.scc += E1000_READ_REG(&adapter->hw, E1000_SCC);
	adapter->stats.ecol += E1000_READ_REG(&adapter->hw, E1000_ECOL);

	adapter->stats.mcc += E1000_READ_REG(&adapter->hw, E1000_MCC);
	adapter->stats.latecol += E1000_READ_REG(&adapter->hw, E1000_LATECOL);
	adapter->stats.colc += E1000_READ_REG(&adapter->hw, E1000_COLC);
	adapter->stats.dc += E1000_READ_REG(&adapter->hw, E1000_DC);
	adapter->stats.rlec += E1000_READ_REG(&adapter->hw, E1000_RLEC);
	adapter->stats.xonrxc += E1000_READ_REG(&adapter->hw, E1000_XONRXC);
	adapter->stats.xontxc += E1000_READ_REG(&adapter->hw, E1000_XONTXC);
	adapter->stats.xoffrxc += E1000_READ_REG(&adapter->hw, E1000_XOFFRXC);
	adapter->stats.xofftxc += E1000_READ_REG(&adapter->hw, E1000_XOFFTXC);
	adapter->stats.fcruc += E1000_READ_REG(&adapter->hw, E1000_FCRUC);
	adapter->stats.prc64 += E1000_READ_REG(&adapter->hw, E1000_PRC64);
	adapter->stats.prc127 += E1000_READ_REG(&adapter->hw, E1000_PRC127);
	adapter->stats.prc255 += E1000_READ_REG(&adapter->hw, E1000_PRC255);
	adapter->stats.prc511 += E1000_READ_REG(&adapter->hw, E1000_PRC511);
	adapter->stats.prc1023 += E1000_READ_REG(&adapter->hw, E1000_PRC1023);
	adapter->stats.prc1522 += E1000_READ_REG(&adapter->hw, E1000_PRC1522);
	adapter->stats.gprc += E1000_READ_REG(&adapter->hw, E1000_GPRC);
	adapter->stats.bprc += E1000_READ_REG(&adapter->hw, E1000_BPRC);
	adapter->stats.mprc += E1000_READ_REG(&adapter->hw, E1000_MPRC);
	adapter->stats.gptc += E1000_READ_REG(&adapter->hw, E1000_GPTC);

	/* For the 64-bit byte counters the low dword must be read first. */
	/* Both registers clear on the read of the high dword */

	adapter->stats.gorc += E1000_READ_REG(&adapter->hw, E1000_GORCH);
	adapter->stats.gotc += E1000_READ_REG(&adapter->hw, E1000_GOTCH);

	adapter->stats.rnbc += E1000_READ_REG(&adapter->hw, E1000_RNBC);
	adapter->stats.ruc += E1000_READ_REG(&adapter->hw, E1000_RUC);
	adapter->stats.rfc += E1000_READ_REG(&adapter->hw, E1000_RFC);
	adapter->stats.roc += E1000_READ_REG(&adapter->hw, E1000_ROC);
	adapter->stats.rjc += E1000_READ_REG(&adapter->hw, E1000_RJC);

	adapter->stats.tor += E1000_READ_REG(&adapter->hw, E1000_TORH);
	adapter->stats.tot += E1000_READ_REG(&adapter->hw, E1000_TOTH);

	adapter->stats.tpr += E1000_READ_REG(&adapter->hw, E1000_TPR);
	adapter->stats.tpt += E1000_READ_REG(&adapter->hw, E1000_TPT);
	adapter->stats.ptc64 += E1000_READ_REG(&adapter->hw, E1000_PTC64);
	adapter->stats.ptc127 += E1000_READ_REG(&adapter->hw, E1000_PTC127);
	adapter->stats.ptc255 += E1000_READ_REG(&adapter->hw, E1000_PTC255);
	adapter->stats.ptc511 += E1000_READ_REG(&adapter->hw, E1000_PTC511);
	adapter->stats.ptc1023 += E1000_READ_REG(&adapter->hw, E1000_PTC1023);
	adapter->stats.ptc1522 += E1000_READ_REG(&adapter->hw, E1000_PTC1522);
	adapter->stats.mptc += E1000_READ_REG(&adapter->hw, E1000_MPTC);
	adapter->stats.bptc += E1000_READ_REG(&adapter->hw, E1000_BPTC);

	if (adapter->hw.mac.type >= e1000_82543) {
		adapter->stats.algnerrc += 
		E1000_READ_REG(&adapter->hw, E1000_ALGNERRC);
		adapter->stats.rxerrc += 
		E1000_READ_REG(&adapter->hw, E1000_RXERRC);
		adapter->stats.tncrs += 
		E1000_READ_REG(&adapter->hw, E1000_TNCRS);
		adapter->stats.cexterr += 
		E1000_READ_REG(&adapter->hw, E1000_CEXTERR);
		adapter->stats.tsctc += 
		E1000_READ_REG(&adapter->hw, E1000_TSCTC);
		adapter->stats.tsctfc += 
		E1000_READ_REG(&adapter->hw, E1000_TSCTFC);
	}

	ifp->if_collisions = adapter->stats.colc;

	/* Rx Errors */
	ifp->if_ierrors =
	    adapter->dropped_pkts + adapter->stats.rxerrc +
	    adapter->stats.crcerrs + adapter->stats.algnerrc +
	    adapter->stats.ruc + adapter->stats.roc +
	    adapter->stats.mpc + adapter->stats.cexterr;

	/* Tx Errors */
	ifp->if_oerrors =
	    adapter->stats.ecol + adapter->stats.latecol +
	    adapter->watchdog_events;
}

static void
em_print_debug_info(struct adapter *adapter)
{
	device_t dev = adapter->dev;
	uint8_t *hw_addr = adapter->hw.hw_addr;

	device_printf(dev, "Adapter hardware address = %p \n", hw_addr);
	device_printf(dev, "CTRL = 0x%x RCTL = 0x%x \n",
	    E1000_READ_REG(&adapter->hw, E1000_CTRL),
	    E1000_READ_REG(&adapter->hw, E1000_RCTL));
	device_printf(dev, "Packet buffer = Tx=%dk Rx=%dk \n",
	    ((E1000_READ_REG(&adapter->hw, E1000_PBA) & 0xffff0000) >> 16),\
	    (E1000_READ_REG(&adapter->hw, E1000_PBA) & 0xffff) );
	device_printf(dev, "Flow control watermarks high = %d low = %d\n",
	    adapter->hw.fc.high_water,
	    adapter->hw.fc.low_water);
	device_printf(dev, "tx_int_delay = %d, tx_abs_int_delay = %d\n",
	    E1000_READ_REG(&adapter->hw, E1000_TIDV),
	    E1000_READ_REG(&adapter->hw, E1000_TADV));
	device_printf(dev, "rx_int_delay = %d, rx_abs_int_delay = %d\n",
	    E1000_READ_REG(&adapter->hw, E1000_RDTR),
	    E1000_READ_REG(&adapter->hw, E1000_RADV));
	device_printf(dev, "fifo workaround = %lld, fifo_reset_count = %lld\n",
	    (long long)adapter->tx_fifo_wrk_cnt,
	    (long long)adapter->tx_fifo_reset_cnt);
	device_printf(dev, "hw tdh = %d, hw tdt = %d\n",
	    E1000_READ_REG(&adapter->hw, E1000_TDH(0)),
	    E1000_READ_REG(&adapter->hw, E1000_TDT(0)));
	device_printf(dev, "hw rdh = %d, hw rdt = %d\n",
	    E1000_READ_REG(&adapter->hw, E1000_RDH(0)),
	    E1000_READ_REG(&adapter->hw, E1000_RDT(0)));
	device_printf(dev, "Num Tx descriptors avail = %d\n",
	    adapter->num_tx_desc_avail);
	device_printf(dev, "Tx Descriptors not avail1 = %ld\n",
	    adapter->no_tx_desc_avail1);
	device_printf(dev, "Tx Descriptors not avail2 = %ld\n",
	    adapter->no_tx_desc_avail2);
	device_printf(dev, "Std mbuf failed = %ld\n",
	    adapter->mbuf_alloc_failed);
	device_printf(dev, "Std mbuf cluster failed = %ld\n",
	    adapter->mbuf_cluster_failed);
	device_printf(dev, "Driver dropped packets = %ld\n",
	    adapter->dropped_pkts);
	device_printf(dev, "Driver tx dma failure in encap = %ld\n",
	    adapter->no_tx_dma_setup);

	device_printf(dev, "TXCSUM try pullup = %lu\n",
	    adapter->tx_csum_try_pullup);
	device_printf(dev, "TXCSUM m_pullup(eh) called = %lu\n",
	    adapter->tx_csum_pullup1);
	device_printf(dev, "TXCSUM m_pullup(eh) failed = %lu\n",
	    adapter->tx_csum_pullup1_failed);
	device_printf(dev, "TXCSUM m_pullup(eh+ip) called = %lu\n",
	    adapter->tx_csum_pullup2);
	device_printf(dev, "TXCSUM m_pullup(eh+ip) failed = %lu\n",
	    adapter->tx_csum_pullup2_failed);
	device_printf(dev, "TXCSUM non-writable(eh) droped = %lu\n",
	    adapter->tx_csum_drop1);
	device_printf(dev, "TXCSUM non-writable(eh+ip) droped = %lu\n",
	    adapter->tx_csum_drop2);
}

static void
em_print_hw_stats(struct adapter *adapter)
{
	device_t dev = adapter->dev;

	device_printf(dev, "Excessive collisions = %lld\n",
	    (long long)adapter->stats.ecol);
#if (DEBUG_HW > 0)  /* Dont output these errors normally */
	device_printf(dev, "Symbol errors = %lld\n",
	    (long long)adapter->stats.symerrs);
#endif
	device_printf(dev, "Sequence errors = %lld\n",
	    (long long)adapter->stats.sec);
	device_printf(dev, "Defer count = %lld\n",
	    (long long)adapter->stats.dc);
	device_printf(dev, "Missed Packets = %lld\n",
	    (long long)adapter->stats.mpc);
	device_printf(dev, "Receive No Buffers = %lld\n",
	    (long long)adapter->stats.rnbc);
	/* RLEC is inaccurate on some hardware, calculate our own. */
	device_printf(dev, "Receive Length Errors = %lld\n",
	    ((long long)adapter->stats.roc + (long long)adapter->stats.ruc));
	device_printf(dev, "Receive errors = %lld\n",
	    (long long)adapter->stats.rxerrc);
	device_printf(dev, "Crc errors = %lld\n",
	    (long long)adapter->stats.crcerrs);
	device_printf(dev, "Alignment errors = %lld\n",
	    (long long)adapter->stats.algnerrc);
	device_printf(dev, "Collision/Carrier extension errors = %lld\n",
	    (long long)adapter->stats.cexterr);
	device_printf(dev, "RX overruns = %ld\n", adapter->rx_overruns);
	device_printf(dev, "watchdog timeouts = %ld\n",
	    adapter->watchdog_events);
	device_printf(dev, "XON Rcvd = %lld\n",
	    (long long)adapter->stats.xonrxc);
	device_printf(dev, "XON Xmtd = %lld\n",
	    (long long)adapter->stats.xontxc);
	device_printf(dev, "XOFF Rcvd = %lld\n",
	    (long long)adapter->stats.xoffrxc);
	device_printf(dev, "XOFF Xmtd = %lld\n",
	    (long long)adapter->stats.xofftxc);
	device_printf(dev, "Good Packets Rcvd = %lld\n",
	    (long long)adapter->stats.gprc);
	device_printf(dev, "Good Packets Xmtd = %lld\n",
	    (long long)adapter->stats.gptc);
}

static void
em_print_nvm_info(struct adapter *adapter)
{
	uint16_t	eeprom_data;
	int	i, j, row = 0;

	/* Its a bit crude, but it gets the job done */
	kprintf("\nInterface EEPROM Dump:\n");
	kprintf("Offset\n0x0000  ");
	for (i = 0, j = 0; i < 32; i++, j++) {
		if (j == 8) { /* Make the offset block */
			j = 0; ++row;
			kprintf("\n0x00%x0  ",row);
		}
		e1000_read_nvm(&adapter->hw, i, 1, &eeprom_data);
		kprintf("%04x ", eeprom_data);
	}
	kprintf("\n");
}

static int
em_sysctl_debug_info(SYSCTL_HANDLER_ARGS)
{
	struct adapter *adapter;
	struct ifnet *ifp;
	int error, result;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);
	if (error || !req->newptr)
		return (error);

	adapter = (struct adapter *)arg1;
	ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	if (result == 1)
		em_print_debug_info(adapter);

	/*
	 * This value will cause a hex dump of the
	 * first 32 16-bit words of the EEPROM to
	 * the screen.
	 */
	if (result == 2)
		em_print_nvm_info(adapter);

	lwkt_serialize_exit(ifp->if_serializer);

	return (error);
}

static int
em_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	int error, result;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);
	if (error || !req->newptr)
		return (error);

	if (result == 1) {
		struct adapter *adapter = (struct adapter *)arg1;
		struct ifnet *ifp = &adapter->arpcom.ac_if;

		lwkt_serialize_enter(ifp->if_serializer);
		em_print_hw_stats(adapter);
		lwkt_serialize_exit(ifp->if_serializer);
	}
	return (error);
}

static int
em_sysctl_int_delay(SYSCTL_HANDLER_ARGS)
{
	struct em_int_delay_info *info;
	struct adapter *adapter;
	struct ifnet *ifp;
	uint32_t regval;
	int error, usecs, ticks;

	info = (struct em_int_delay_info *)arg1;
	usecs = info->value;
	error = sysctl_handle_int(oidp, &usecs, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (usecs < 0 || usecs > EM_TICKS_TO_USECS(65535))
		return (EINVAL);
	info->value = usecs;
	ticks = EM_USECS_TO_TICKS(usecs);

	adapter = info->adapter;
	ifp = &adapter->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	regval = E1000_READ_OFFSET(&adapter->hw, info->offset);
	regval = (regval & ~0xffff) | (ticks & 0xffff);
	/* Handle a few special cases. */
	switch (info->offset) {
	case E1000_RDTR:
		break;

	case E1000_TIDV:
		if (ticks == 0) {
			adapter->txd_cmd &= ~E1000_TXD_CMD_IDE;
			/* Don't write 0 into the TIDV register. */
			regval++;
		} else {
			adapter->txd_cmd |= E1000_TXD_CMD_IDE;
		}
		break;
	}
	E1000_WRITE_OFFSET(&adapter->hw, info->offset, regval);

	lwkt_serialize_exit(ifp->if_serializer);
	return (0);
}

static void
em_add_int_delay_sysctl(struct adapter *adapter, const char *name,
	const char *description, struct em_int_delay_info *info,
	int offset, int value)
{
	info->adapter = adapter;
	info->offset = offset;
	info->value = value;

	if (adapter->sysctl_tree != NULL) {
		SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, name, CTLTYPE_INT|CTLFLAG_RW,
		    info, 0, em_sysctl_int_delay, "I", description);
	}
}

static void
em_add_sysctl(struct adapter *adapter)
{
#ifdef PROFILE_SERIALIZER
	struct ifnet *ifp = &adapter->arpcom.ac_if;
#endif

	sysctl_ctx_init(&adapter->sysctl_ctx);
	adapter->sysctl_tree = SYSCTL_ADD_NODE(&adapter->sysctl_ctx,
					SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO,
					device_get_nameunit(adapter->dev),
					CTLFLAG_RD, 0, "");
	if (adapter->sysctl_tree == NULL) {
		device_printf(adapter->dev, "can't add sysctl node\n");
	} else {
		SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "debug", CTLTYPE_INT|CTLFLAG_RW, adapter, 0,
		    em_sysctl_debug_info, "I", "Debug Information");

		SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "stats", CTLTYPE_INT|CTLFLAG_RW, adapter, 0,
		    em_sysctl_stats, "I", "Statistics");

		SYSCTL_ADD_INT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "rxd", CTLFLAG_RD,
		    &adapter->num_rx_desc, 0, NULL);
		SYSCTL_ADD_INT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "txd", CTLFLAG_RD,
		    &adapter->num_tx_desc, 0, NULL);

#ifdef PROFILE_SERIALIZER
		SYSCTL_ADD_UINT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "serializer_sleep", CTLFLAG_RW,
		    &ifp->if_serializer->sleep_cnt, 0, NULL);
		SYSCTL_ADD_UINT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "serializer_tryfail", CTLFLAG_RW,
		    &ifp->if_serializer->tryfail_cnt, 0, NULL);
		SYSCTL_ADD_UINT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "serializer_enter", CTLFLAG_RW,
		    &ifp->if_serializer->enter_cnt, 0, NULL);
		SYSCTL_ADD_UINT(&adapter->sysctl_ctx,
		    SYSCTL_CHILDREN(adapter->sysctl_tree),
		    OID_AUTO, "serializer_try", CTLFLAG_RW,
		    &ifp->if_serializer->try_cnt, 0, NULL);
#endif
		if (adapter->hw.mac.type >= e1000_82540) {
			SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
			    SYSCTL_CHILDREN(adapter->sysctl_tree),
			    OID_AUTO, "int_throttle_ceil",
			    CTLTYPE_INT|CTLFLAG_RW, adapter, 0,
			    em_sysctl_int_throttle, "I",
			    "interrupt throttling rate");
		}
	}

	/* Set up some sysctls for the tunable interrupt delays */
	em_add_int_delay_sysctl(adapter, "rx_int_delay",
	    "receive interrupt delay in usecs", &adapter->rx_int_delay,
	    E1000_REGISTER(&adapter->hw, E1000_RDTR), em_rx_int_delay_dflt);
	em_add_int_delay_sysctl(adapter, "tx_int_delay",
	    "transmit interrupt delay in usecs", &adapter->tx_int_delay,
	    E1000_REGISTER(&adapter->hw, E1000_TIDV), em_tx_int_delay_dflt);
	if (adapter->hw.mac.type >= e1000_82540) {
		em_add_int_delay_sysctl(adapter, "rx_abs_int_delay",
		    "receive interrupt delay limit in usecs",
		    &adapter->rx_abs_int_delay,
		    E1000_REGISTER(&adapter->hw, E1000_RADV),
		    em_rx_abs_int_delay_dflt);
		em_add_int_delay_sysctl(adapter, "tx_abs_int_delay",
		    "transmit interrupt delay limit in usecs",
		    &adapter->tx_abs_int_delay,
		    E1000_REGISTER(&adapter->hw, E1000_TADV),
		    em_tx_abs_int_delay_dflt);
	}
}

static int
em_sysctl_int_throttle(SYSCTL_HANDLER_ARGS)
{
	struct adapter *adapter = (void *)arg1;
	struct ifnet *ifp = &adapter->arpcom.ac_if;
	int error, throttle;

	throttle = adapter->int_throttle_ceil;
	error = sysctl_handle_int(oidp, &throttle, 0, req);
	if (error || req->newptr == NULL)
		return error;
	if (throttle < 0 || throttle > 1000000000 / 256)
		return EINVAL;

	lwkt_serialize_enter(ifp->if_serializer);

	if (throttle) {
		/*
		 * Set the interrupt throttling rate in 256ns increments,
		 * recalculate sysctl value assignment to get exact frequency.
		 */
		throttle = 1000000000 / 256 / throttle;
		adapter->int_throttle_ceil = 1000000000 / 256 / throttle;
	} else {
		adapter->int_throttle_ceil = 0;
	}
	E1000_WRITE_REG(&adapter->hw, E1000_ITR, throttle);

	lwkt_serialize_exit(ifp->if_serializer);

	if (bootverbose) {
		if_printf(ifp, "Interrupt moderation set to %d/sec\n",
			  adapter->int_throttle_ceil);
	}
	return 0;
}
