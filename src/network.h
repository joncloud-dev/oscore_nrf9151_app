/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight LTE link module for the OSCORE nRF9151 sample.
 *
 * This is a framework-free take on the Asset-Tracker-Template network module:
 * it owns the modem library and LTE Link Control, tracks the link state from
 * the modem's default-PDN events (so "connected" means an IP bearer is actually
 * usable, not merely that the modem registered), forwards the interesting
 * lte_lc events to a single application callback, and provides an escalating
 * recovery ladder. Unlike ATT there is no zbus / SMF / dedicated thread: events
 * arrive on the lte_lc workqueue and the plain main() loop drives connect/send
 * by polling network_is_connected() / blocking on network_wait_connected().
 */

#ifndef NETWORK_H_
#define NETWORK_H_

#include <zephyr/kernel.h>
#include <modem/lte_lc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Events delivered to the application callback. The link-state transitions
 * (CONNECTED/DISCONNECTED) are also reflected by network_is_connected() and
 * network_wait_connected(); the rest are informational. */
enum network_event {
	/* The default PDN is active: an IP bearer is available. */
	NETWORK_EVENT_CONNECTED,

	/* The default PDN went down (deactivated, suspended or detached), or the
	 * modem left the network. The transport should stop until reconnected. */
	NETWORK_EVENT_DISCONNECTED,

	/* The modem reported a SIM error (LTE_LC_NW_REG_UICC_FAIL). */
	NETWORK_EVENT_UICC_FAILURE,

	/* A network attach request was rejected (not registered). */
	NETWORK_EVENT_ATTACH_REJECTED,

	/* The modem detected an attach reset loop and will back off for ~30 min. */
	NETWORK_EVENT_MODEM_RESET_LOOP,

	/* A light (cell-history-based) network search finished without success;
	 * the modem continues with a full search unless stopped. */
	NETWORK_EVENT_LIGHT_SEARCH_DONE,

	/* A full network search finished without finding a suitable cell. */
	NETWORK_EVENT_SEARCH_DONE,

	/* Negotiated PSM parameters changed; see evt->psm_cfg. */
	NETWORK_EVENT_PSM_UPDATE,

	/* Negotiated eDRX parameters changed; see evt->edrx_cfg. */
	NETWORK_EVENT_EDRX_UPDATE,
};

struct network_evt {
	enum network_event type;
	union {
		/* Valid for NETWORK_EVENT_PSM_UPDATE. */
		IF_ENABLED(CONFIG_LTE_LC_PSM_MODULE, (struct lte_lc_psm_cfg psm_cfg));

		/* Valid for NETWORK_EVENT_EDRX_UPDATE. */
		IF_ENABLED(CONFIG_LTE_LC_EDRX_MODULE, (struct lte_lc_edrx_cfg edrx_cfg));
	};
};

/* Application event callback. Invoked from the lte_lc workqueue context, so
 * keep it short and non-blocking. May be NULL if the app only cares about the
 * link state via network_is_connected() / network_wait_connected(). */
typedef void (*network_evt_handler_t)(const struct network_evt *evt, void *user);

/* Initialise the modem library and LTE Link Control, register the event
 * handler, enable default-PDN / modem event notifications and start an
 * asynchronous network attach. Returns 0 on success or a negative errno.
 * The link comes up asynchronously; wait for it with network_wait_connected(). */
int network_init(network_evt_handler_t handler, void *user);

/* True while the default PDN is active (IP bearer usable). Lock-free snapshot
 * intended for the "is it worth trying?" checks in the main loop. */
bool network_is_connected(void);

/* Block until the link is connected, or until the timeout elapses.
 * Returns true if the link is up on return. */
bool network_wait_connected(k_timeout_t timeout);

/* Escalating recovery ladder, keyed on the number of consecutive failures:
 *   - few failures: wait for the modem to re-attach on its own;
 *   - more failures: force an offline/normal re-attach;
 *   - persistent failures: shut down and reinitialise the modem library.
 * After acting it waits (bounded) for the link to return so the caller does
 * not spin.
 *
 * The caller MUST close any open transport (e.g. oscore_cloud_disconnect())
 * before calling this: a full modem-library reinit requires all sockets to be
 * closed. */
void network_recover(unsigned int failures);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H_ */
