/*
 * Copyright (c) Michael Tharp <gxti@partiallystapled.com>
 *
 * This file is distributed under the terms of the MIT License.
 * See the LICENSE file at the top of this tree, or if it is missing a copy can
 * be found at http://opensource.org/licenses/MIT
 */

#include "common.h"
#include "cmdline.h"
#include "eeprom.h"
#include "eth_mac.h"
#include "ntpserver.h"
#include "tcpip.h"

#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/stats.h"
#include "lwip/timers.h"
#include "netif/etharp.h"

#define TCPIP_STACK 512
OS_STK tcpip_stack[TCPIP_STACK];
OS_TID tcpip_tid;

OS_TCID timer;
OS_FlagID timer_flag;

struct netif thisif;

static void tcpip_thread(void *p);
static void configure_interface(void);
static err_t ethernetif_init(struct netif *netif);
static err_t low_level_output(struct netif *netif, struct pbuf *p);
static void ethernetif_input(struct netif *netif);
static void tcpip_timer(void);


void
tcpip_start(void) {
	ASSERT((timer_flag = CoCreateFlag(1, 0)) != E_CREATE_FAIL);
	timer = CoCreateTmr(TMR_TYPE_PERIODIC, S2ST(1), S2ST(1), tcpip_timer);
	ASSERT(timer != E_CREATE_FAIL);
	CoStartTmr(timer);

	tcpip_tid = CoCreateTask(tcpip_thread, NULL, THREAD_PRIO_TCPIP,
			&tcpip_stack[TCPIP_STACK-1], TCPIP_STACK);
	ASSERT(tcpip_tid != E_CREATE_FAIL);
}


static void
tcpip_timer(void) {
	isr_SetFlag(timer_flag);
}


static void
tcpip_thread(void *p) {
	uint32_t flags;
	StatusType rc;
	lwip_init();
	configure_interface();
	ntp_server_start();
	while (1) {
		flags = CoWaitForMultipleFlags(0
				| (1 << timer_flag)
				| (1 << mac_rx_flag)
				, OPT_WAIT_ANY, 0, &rc);
		ASSERT(rc == E_OK);
		if (flags & (1 << timer_flag)) {
			if (smi_poll_link_status()) {
				if (!netif_is_link_up(&thisif)) {
					no_cli_puts("Ethernet link up\r\n");
					netif_set_link_up(&thisif);
				}
				GPIO_OFF(ETH_LED);
			} else {
				if (netif_is_link_up(&thisif)) {
					no_cli_puts("Ethernet link is down\r\n");
					netif_set_link_down(&thisif);
				}
				GPIO_ON(ETH_LED);
			}
			sys_check_timeouts();
		}
		if (flags & (1 << mac_rx_flag)) {
			ethernetif_input(&thisif);
		}
	}
}


static void
configure_interface(void) {
	struct ip_addr ip, gateway, netmask;
	ASSERT(eeprom_read(0xFA, thisif.hwaddr, 6) == E_OK);
	mac_start();
	mac_set_hwaddr(thisif.hwaddr);

	ip.addr = cfg.ip_addr;
	gateway.addr = cfg.ip_gateway;
	netmask.addr = cfg.ip_netmask;
	netif_add(&thisif, &ip, &netmask, &gateway, NULL, ethernetif_init, ethernet_input);

	netif_set_default(&thisif);
	netif_set_up(&thisif);
	if (ip.addr == 0 || netmask.addr == 0) {
		dhcp_start(&thisif);
	}
}


static err_t
ethernetif_init(struct netif *netif) {
	netif->state = NULL;
	netif->name[0] = 'e';
	netif->name[1] = 'n';
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;
	netif->hwaddr_len = ETHARP_HWADDR_LEN;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
	return ERR_OK;
}


static err_t
low_level_output(struct netif *netif, struct pbuf *p) {
	struct pbuf *q;
	mac_desc_t *tdes;
	tdes = mac_get_tx_descriptor(MS2ST(50));
	if (tdes == NULL) {
		return ERR_TIMEOUT;
	}
	pbuf_header(p, -ETH_PAD_SIZE);
	for (q = p; q != NULL; q = q->next) {
		mac_write_tx_descriptor(tdes, q->payload, q->len);
	}
	mac_release_tx_descriptor(tdes);
	pbuf_header(p, ETH_PAD_SIZE);
	LINK_STATS_INC(link.xmit);
	return ERR_OK;
}


static void
ethernetif_input(struct netif *netif) {
	struct pbuf *p, *q;
	uint16_t len;
	mac_desc_t *rdesc;

	(void)netif;
	if ((rdesc = mac_get_rx_descriptor()) == NULL) {
		return;
	}
	len = rdesc->size + ETH_PAD_SIZE;
	if ((p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL)) == NULL) {
		mac_release_rx_descriptor(rdesc);
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		return;
	}
	pbuf_header(p, -ETH_PAD_SIZE);
	for (q = p; q != NULL; q = q->next) {
		mac_read_rx_descriptor(rdesc, q->payload, q->len);
	}
	mac_release_rx_descriptor(rdesc);
	pbuf_header(p, ETH_PAD_SIZE);
	LINK_STATS_INC(link.recv);
	if (netif->input(p, netif) != ERR_OK) {
		pbuf_free(p);
	}
}