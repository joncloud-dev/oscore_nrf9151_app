/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight LTE link module for the OSCORE nRF9151 sample. See network.h for
 * the rationale and the public contract.
 *
 * Link-state model (mirrors the Asset-Tracker-Template network module):
 *   - "connected" is driven by the modem's DEFAULT PDN events, not by raw
 *     network-registration status. LTE_LC_EVT_PDN_ACTIVATED / _RESUMED mean the
 *     IP bearer is usable; _DEACTIVATED / _SUSPENDED / _NETWORK_DETACH mean it
 *     is not. This is stricter (and more correct) than "registered home", which
 *     can be true before the default context is actually up.
 *   - network-registration events are used only to surface failures (SIM error,
 *     attach rejected); modem events surface reset loops and search results.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>

#include "network.h"

LOG_MODULE_REGISTER(network, LOG_LEVEL_INF);

/* Level-triggered link-up signal so the main loop can both poll (link_ready)
 * and block on it (network_wait_connected). */
#define NET_EVT_LINK_UP BIT(0)

static K_EVENT_DEFINE(net_evt);

/* Fast, lock-free snapshot of the link state. The authoritative signal is
 * net_evt; this just avoids blocking in the hot path. */
static atomic_t link_ready = ATOMIC_INIT(0);

static network_evt_handler_t app_handler;
static void *app_user;

static void notify_app(const struct network_evt *evt)
{
	if (app_handler) {
		app_handler(evt, app_user);
	}
}

static void set_link_up(bool up)
{
	bool was_up = atomic_set(&link_ready, up ? 1 : 0) != 0;

	if (up) {
		k_event_post(&net_evt, NET_EVT_LINK_UP);
	} else {
		k_event_clear(&net_evt, NET_EVT_LINK_UP);
	}

	/* Only emit an event on an actual edge so repeated PDN activations /
	 * deactivations do not spam the application. */
	if (up != was_up) {
		struct network_evt evt = {
			.type = up ? NETWORK_EVENT_CONNECTED : NETWORK_EVENT_DISCONNECTED,
		};

		LOG_INF("LTE link %s", up ? "up (default PDN active)" : "down");
		notify_app(&evt);
	}
}

static void notify_simple(enum network_event type)
{
	struct network_evt evt = { .type = type };

	notify_app(&evt);
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		/* Registration status is used only for failure reporting; the link
		 * up/down transition is driven by the PDN events below. */
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
			notify_simple(NETWORK_EVENT_UICC_FAILURE);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered; check rejection cause");
			notify_simple(NETWORK_EVENT_ATTACH_REJECTED);
		}
		break;

#if defined(CONFIG_LTE_LC_PDN_MODULE)
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
		case LTE_LC_EVT_PDN_RESUMED:
			set_link_up(true);
			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
		case LTE_LC_EVT_PDN_SUSPENDED:
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			set_link_up(false);
			break;
		default:
			break;
		}
		break;
#endif /* CONFIG_LTE_LC_PDN_MODULE */

	case LTE_LC_EVT_MODEM_EVENT:
		switch (evt->modem_evt.type) {
		case LTE_LC_MODEM_EVT_RESET_LOOP:
			LOG_WRN("Modem detected an attach reset loop");
			notify_simple(NETWORK_EVENT_MODEM_RESET_LOOP);
			break;
		case LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE:
			notify_simple(NETWORK_EVENT_LIGHT_SEARCH_DONE);
			break;
		case LTE_LC_MODEM_EVT_SEARCH_DONE:
			notify_simple(NETWORK_EVENT_SEARCH_DONE);
			break;
		default:
			break;
		}
		break;

#if defined(CONFIG_LTE_LC_PSM_MODULE)
	case LTE_LC_EVT_PSM_UPDATE: {
		struct network_evt out = {
			.type = NETWORK_EVENT_PSM_UPDATE,
			.psm_cfg = evt->psm_cfg,
		};

		LOG_INF("PSM update: TAU %d s, active time %d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		notify_app(&out);
		break;
	}
#endif

#if defined(CONFIG_LTE_LC_EDRX_MODULE)
	case LTE_LC_EVT_EDRX_UPDATE: {
		struct network_evt out = {
			.type = NETWORK_EVENT_EDRX_UPDATE,
			.edrx_cfg = evt->edrx_cfg,
		};

		LOG_INF("eDRX update: mode %d, eDRX %.02f s, PTW %.02f s",
			evt->edrx_cfg.mode, (double)evt->edrx_cfg.edrx,
			(double)evt->edrx_cfg.ptw);
		notify_app(&out);
		break;
	}
#endif

	default:
		break;
	}
}

/* Initialise (or re-initialise) the modem library, handling the delta-firmware
 * update result codes that nrf_modem_lib_init() can return. Returns 0 when the
 * modem is usable, negative on a fatal error. */
static int modem_lib_init(void)
{
	int err = nrf_modem_lib_init();

	if (err < 0) {
		LOG_ERR("nrf_modem_lib_init failed: %d", err);
		return err;
	}

	/* A positive return means a scheduled modem (delta) firmware update was
	 * applied during init. OK / AUTH / UUID / VOLTAGE_LOW all leave the modem
	 * initialised and running; only the fatal errors are unrecoverable. */
	switch (err) {
	case 0:
		break;
	case NRF_MODEM_DFU_RESULT_OK:
		LOG_INF("Modem firmware update applied successfully");
		break;
	case NRF_MODEM_DFU_RESULT_AUTH_ERROR:
	case NRF_MODEM_DFU_RESULT_UUID_ERROR:
		LOG_WRN("Modem firmware update failed (0x%x); running previous firmware", err);
		break;
	case NRF_MODEM_DFU_RESULT_VOLTAGE_LOW:
		LOG_WRN("Modem firmware update deferred (low voltage); retry on reboot");
		break;
	default:
		LOG_ERR("Modem firmware update fatal error 0x%x", err);
		return -EIO;
	}

	return 0;
}

/* Register the event handler, enable the notifications we react to, and start
 * an asynchronous attach. Safe to call again after a modem reinit. */
static int link_start(void)
{
	int err;

	lte_lc_register_handler(lte_lc_evt_handler);

	/* Default-PDN activate/deactivate events: the authoritative link signal.
	 * (Modem reset-loop / search-done notifications are enabled automatically
	 * by the lte_lc CFUN hook when CONFIG_LTE_LC_MODEM_EVENTS_MODULE=y.) */
	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable failed: %d", err);
		return err;
	}

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed: %d", err);
		return err;
	}

	return 0;
}

int network_init(network_evt_handler_t handler, void *user)
{
	int err;

	app_handler = handler;
	app_user = user;

	LOG_INF("Bringing up the modem...");

	err = modem_lib_init();
	if (err) {
		return err;
	}

	return link_start();
}

bool network_is_connected(void)
{
	return atomic_get(&link_ready) != 0;
}

bool network_wait_connected(k_timeout_t timeout)
{
	if (network_is_connected()) {
		return true;
	}

	return k_event_wait(&net_evt, NET_EVT_LINK_UP, false, timeout) != 0;
}

/* Bounded exponential backoff for pacing recovery attempts: 8, 16, 32 ...
 * capped so a device that stays out of coverage does not sleep for hours. */
static uint32_t recovery_backoff_seconds(unsigned int failures)
{
	const uint32_t base = 8;
	const uint32_t cap = 300;
	unsigned int shift = MIN(failures, 6U);
	uint32_t backoff = base << shift;

	return MIN(backoff, cap);
}

/* Stage 2: drop and re-attach, nudging the modem out of a stuck search / denied
 * state without a full library teardown. */
static void lte_offline_normal_cycle(void)
{
	int err;

	LOG_WRN("Link recovery: forcing an LTE offline/normal re-attach");

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline failed: %d", err);
	}

	k_sleep(K_SECONDS(1));

	err = lte_lc_normal();
	if (err) {
		LOG_ERR("lte_lc_normal failed: %d", err);
	}
}

/* Stage 3: tear the modem library down and bring it back up, the heaviest local
 * recovery short of a reboot. The caller must have closed all sockets. */
static void modem_reinit(void)
{
	int err;

	LOG_WRN("Link recovery: reinitialising the modem library");

	(void)lte_lc_offline();

	err = nrf_modem_lib_shutdown();
	if (err) {
		LOG_ERR("nrf_modem_lib_shutdown failed: %d", err);
	}

	set_link_up(false);

	err = modem_lib_init();
	if (err) {
		LOG_ERR("modem reinit failed: %d", err);
		return;
	}

	(void)link_start();
}

void network_recover(unsigned int failures)
{
	uint32_t backoff = recovery_backoff_seconds(failures);

	LOG_WRN("LTE link unavailable; recovery attempt %u (backoff %u s)",
		failures, backoff);

	if (failures >= CONFIG_APP_MODEM_REINIT_AFTER_FAILURES) {
		k_sleep(K_SECONDS(backoff));
		modem_reinit();
	} else if (failures >= 2) {
		lte_offline_normal_cycle();
	}

	if (!network_wait_connected(K_SECONDS(CONFIG_APP_LINK_TIMEOUT_SECONDS))) {
		LOG_WRN("LTE link still down after %d s; will retry",
			CONFIG_APP_LINK_TIMEOUT_SECONDS);
		k_sleep(K_SECONDS(backoff));
	}
}
