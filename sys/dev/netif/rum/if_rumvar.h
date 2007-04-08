/*	$OpenBSD: if_rumvar.h,v 1.6 2006/08/18 15:11:12 damien Exp $	*/
/*	$DragonFly: src/sys/dev/netif/rum/if_rumvar.h,v 1.3 2007/04/08 09:41:41 sephe Exp $	*/

/*-
 * Copyright (c) 2005, 2006 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2006 Niall O'Higgins <niallo@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define RT2573_RX_LIST_COUNT	1
#define RT2573_TX_LIST_COUNT	1

struct rum_rx_radiotap_header {
	struct ieee80211_radiotap_header wr_ihdr;
	uint8_t		wr_flags;
	uint8_t		wr_rate;
	uint16_t	wr_chan_freq;
	uint16_t	wr_chan_flags;
	uint8_t		wr_antenna;
	uint8_t		wr_antsignal;
} __packed;

#define RT2573_RX_RADIOTAP_PRESENT					\
	((1 << IEEE80211_RADIOTAP_FLAGS) |				\
	 (1 << IEEE80211_RADIOTAP_RATE) |				\
	 (1 << IEEE80211_RADIOTAP_CHANNEL) |				\
	 (1 << IEEE80211_RADIOTAP_ANTENNA) |				\
	 (1 << IEEE80211_RADIOTAP_DB_ANTSIGNAL))

struct rum_tx_radiotap_header {
	struct ieee80211_radiotap_header wt_ihdr;
	uint8_t		wt_flags;
	uint8_t		wt_rate;
	uint16_t	wt_chan_freq;
	uint16_t	wt_chan_flags;
	uint8_t		wt_antenna;
} __packed;

#define RT2573_TX_RADIOTAP_PRESENT						\
	((1 << IEEE80211_RADIOTAP_FLAGS) |				\
	 (1 << IEEE80211_RADIOTAP_RATE) |				\
	 (1 << IEEE80211_RADIOTAP_CHANNEL) |				\
	 (1 << IEEE80211_RADIOTAP_ANTENNA))

struct rum_softc;

struct rum_tx_data {
	struct rum_softc	*sc;
	usbd_xfer_handle	xfer;
	uint8_t			*buf;
	struct mbuf		*m;
	struct ieee80211_node	*ni;
};

struct rum_rx_data {
	struct rum_softc	*sc;
	usbd_xfer_handle	xfer;
	uint8_t			*buf;
	struct mbuf		*m;
};

struct rum_softc {
	USBBASEDEVICE			sc_dev;
	struct ieee80211com		sc_ic;
	int				(*sc_newstate)(struct ieee80211com *,
					    enum ieee80211_state, int);

	uint32_t			sc_flags;
#define RUM_FLAG_SYNCTASK	0x1
#define RUM_FLAG_STOPPED	0x2
#define RUM_FLAG_DETACHED	0x4
#define RUM_FLAG_CONFIG		0x8

	usbd_device_handle		sc_udev;
	usbd_interface_handle		sc_iface;

	struct ieee80211_channel	*sc_curchan;

	int				sc_rx_no;
	int				sc_tx_no;

	uint16_t			macbbp_rev;
	uint8_t				rf_rev;
	uint8_t				rffreq;

	usbd_xfer_handle		stats_xfer;

	usbd_pipe_handle		sc_rx_pipeh;
	usbd_pipe_handle		sc_tx_pipeh;

	enum ieee80211_state		sc_state;
	int				sc_arg;
	int				sc_sifs;
	struct usb_task			sc_task;

	struct ieee80211_ratectl_stats	sc_stats;

	struct rum_rx_data		rx_data[RT2573_RX_LIST_COUNT];
	struct rum_tx_data		tx_data[RT2573_TX_LIST_COUNT];
	int				tx_queued;

	struct callout			scan_ch;
	struct callout			stats_ch;

	int				sc_tx_timer;

	uint32_t			sta[6];
	uint32_t			rf_regs[4];
	uint8_t				txpow[44];

	struct {
		uint8_t	val;
		uint8_t	reg;
	} __packed			bbp_prom[16];

	int				hw_radio;
	int				rx_ant;
	int				tx_ant;
	int				nb_ant;
	int				ext_2ghz_lna;
	int				ext_5ghz_lna;
	int				rssi_2ghz_corr;
	int				rssi_5ghz_corr;
	uint8_t				bbp17;

	struct bpf_if			*sc_drvbpf;

	union {
		struct rum_rx_radiotap_header th;
		uint8_t	pad[64];
	}				sc_rxtapu;
#define sc_rxtap	sc_rxtapu.th
	int				sc_rxtap_len;

	union {
		struct rum_tx_radiotap_header th;
		uint8_t	pad[64];
	}				sc_txtapu;
#define sc_txtap	sc_txtapu.th
	int				sc_txtap_len;
};

/* See the comment take from rt73 near RT2573_TX_LONG_RETRY */
#define RUM_TX_SHORT_RETRY_MAX		7
#define RUM_TX_LONG_RETRY_MAX		4

#define RUM_TX_PKT_NO_RETRY(sc)		(le32toh((sc)->sta[4]) & 0xffff)
#define RUM_TX_PKT_ONE_RETRY(sc)	(le32toh((sc)->sta[4]) >> 16)
#define RUM_TX_PKT_MULTI_RETRY(sc)	(le32toh((sc)->sta[5]) & 0xffff)
#define RUM_TX_PKT_FAIL(sc)		(le32toh((sc)->sta[5]) >> 16)
