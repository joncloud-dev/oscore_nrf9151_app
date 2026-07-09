/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal OSCORE cloud sample for the nRF9151 DK.
 *
 * This is the "batteries included" usage of the oscore_cloud library: no zbus,
 * no state-machine framework, just a plain main() loop driving the
 * cloud_session helper (connect -> send telemetry -> apply shadow delta).
 *
 * Boot flow:
 *   1. Bring up the modem and attach to the LTE network (network module).
 *   2. Best-effort wall-clock sync (for the optional telemetry timestamp).
 *   3. Configure the library and enter the connect / send / backoff loop.
 *
 * Network robustness:
 *   The LTE link is owned by the lightweight network module (see network.c). It
 *   tracks the link state from the modem's default-PDN events -- so "connected"
 *   means an IP bearer is actually usable, not just that the modem registered --
 *   and exposes it via network_is_connected() / network_wait_connected(). The
 *   main loop never touches the cloud transport unless the link is up, and on
 *   persistent failure it asks the module to walk a recovery ladder (wait for
 *   re-attach -> force an offline/normal cycle -> reinitialise the modem
 *   library). Everything stays single-threaded and driven from main(); LTE
 *   events arrive asynchronously on the lte_lc workqueue.
 *
 * OSCORE key material is NOT compiled in: it is provisioned once on the bench
 * with the shell command
 *     oscore provision <secret_hex(16B)> <salt_hex> <sender_id_hex> <recipient_id_hex>
 * (registered by the library) and then loaded from the TF-M secure domain /
 * Protected Storage on every boot. An unprovisioned device logs an error and
 * stays disconnected while the shell remains available.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <date_time.h>

#include <oscore_cloud/cloud_config.h>
#include <oscore_cloud/cloud_session.h>
#include <oscore_cloud/cloud_client.h>
#ifdef CONFIG_OSCORE_CLOUD_DIAG
#include <oscore_cloud/diag_bridge.h>
#endif

#include "network.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static K_SEM_DEFINE(time_ready_sem, 0, 1);

/* Called by the library after a telemetry post that carried a shadow delta.
 * The library acknowledges the desired version automatically once we return;
 * here we just log what the cloud wants us to change.
 */
static void on_shadow_delta(const struct cloud_shadow_delta *delta, void *user)
{
	ARG_UNUSED(user);

	LOG_INF("Shadow delta (desired version %llu): %u entr%s",
		(unsigned long long)delta->desired_version,
		(unsigned int)delta->entry_count, delta->entry_count == 1 ? "y" : "ies");

	for (size_t i = 0; i < delta->entry_count; i++) {
		LOG_INF("  %s = %s", delta->entries[i].key, delta->entries[i].value);
	}
}

/* Network module callback. The connect/send loop reacts to the link state via
 * network_is_connected() / network_wait_connected(); here we only surface the
 * informational events (SIM failure, attach rejected, reset loop, search
 * results, negotiated PSM/eDRX) for visibility. */
static void on_network_event(const struct network_evt *evt, void *user)
{
	ARG_UNUSED(user);

	switch (evt->type) {
	case NETWORK_EVENT_CONNECTED:
	case NETWORK_EVENT_DISCONNECTED:
		/* Link transitions are logged by the module and acted on in main(). */
		break;
	case NETWORK_EVENT_UICC_FAILURE:
		LOG_ERR("SIM failure reported by modem");
		break;
	case NETWORK_EVENT_ATTACH_REJECTED:
		LOG_WRN("Network attach rejected");
		break;
	case NETWORK_EVENT_MODEM_RESET_LOOP:
		LOG_WRN("Modem attach reset loop; backing off");
		break;
	case NETWORK_EVENT_LIGHT_SEARCH_DONE:
		LOG_INF("Light network search done (still searching)");
		break;
	case NETWORK_EVENT_SEARCH_DONE:
		LOG_DBG("Full network search done");
		break;
	case NETWORK_EVENT_PSM_UPDATE:
	case NETWORK_EVENT_EDRX_UPDATE:
		/* Power-saving parameters; logged by the module. */
		break;
	}
}

static void date_time_handler(const struct date_time_evt *evt)
{
	if (evt->type != DATE_TIME_NOT_OBTAINED) {
		k_sem_give(&time_ready_sem);
	}
}

/* Best-effort wall-clock sync. Only runs when the time is not already valid, so
 * it is cheap to call again after a reconnect. Blocks up to 30 s. */
static void ensure_time(void)
{
	int64_t now_ms;

	if (date_time_now(&now_ms) == 0) {
		return;
	}

	k_sem_reset(&time_ready_sem);
	(void)date_time_update_async(NULL);
	if (k_sem_take(&time_ready_sem, K_SECONDS(30)) == 0) {
		LOG_INF("Wall-clock time obtained");
	} else {
		LOG_WRN("Time not obtained; telemetry will omit the timestamp");
	}
}

/* Fill a telemetry record for one check-in: transport class, firmware version,
 * an optional timestamp and a demo uptime metric (free-form key 10). */
static void build_telemetry(struct cloud_telemetry *t)
{
	int64_t now_ms;

	cloud_telemetry_init(t);
	cloud_telemetry_set_transport(t, CLOUD_TRANSPORT_TN);
#ifndef CONFIG_OSCORE_CLOUD_DIAG_BUILD_ID
	/* With the diagnostics build-id enabled, the library reports an
	 * auto-generated "<semver>+<build-id>" firmware version (from the VERSION
	 * file) instead, so leave fw_version unset here and let the bridge fill it. */
	cloud_telemetry_set_fw_version(t, CONFIG_APP_VERSION);
#endif

	if (date_time_now(&now_ms) == 0) {
		cloud_telemetry_set_timestamp(t, (uint64_t)(now_ms / 1000));
	}

	/* Free-form metric (integer key). The server stores any non-well-known
	 * key as a metric; key 10 here is an arbitrary example (device uptime). */
	(void)cloud_telemetry_add_uint(t, 10, (uint64_t)k_uptime_seconds());
}

int main(void)
{
	const uint32_t interval_s = CONFIG_APP_SAMPLE_INTERVAL_SECONDS;
	unsigned int attempts = 0;  /* transport (OSCORE) connect backoff */
	unsigned int failures = 0;  /* consecutive link-recovery escalation */
	bool connected = false;
	int err;

	LOG_INF("OSCORE nRF9151 sample starting");

#ifdef CONFIG_OSCORE_CLOUD_DIAG
	/* Process the retained observability region + hwinfo before anything else
	 * can overwrite it: composes the boot report, recovers a pending crash
	 * snapshot/breadcrumbs, and arms the one-shot device-info report. The
	 * session helper folds these into telemetry automatically. */
	oscore_cloud_diag_init();
#endif

	/* Initialise the settings subsystem up front so the OSCORE replay counter
	 * (Sender Sequence Number) can be restored from NVS. The library also
	 * initialises it lazily, but doing it here keeps boot ordering explicit. */
	err = settings_subsys_init();
	if (err) {
		LOG_WRN("settings_subsys_init failed: %d (SSN will not persist)", err);
	}

	date_time_register_handler(date_time_handler);

	err = network_init(on_network_event, NULL);
	if (err) {
		LOG_ERR("Network bring-up failed (%d); halting", err);
		return err;
	}

	LOG_INF("Waiting for LTE connectivity...");
	if (!network_wait_connected(K_SECONDS(CONFIG_APP_LINK_TIMEOUT_SECONDS))) {
		/* Not fatal: the modem keeps searching in the background and the link
		 * will flip up when the default PDN activates. The main loop waits for
		 * (and recovers) the link before it connects. */
		LOG_WRN("LTE not connected after %d s; continuing",
			CONFIG_APP_LINK_TIMEOUT_SECONDS);
	}

	/* NULL config keeps all compile-time defaults: server IP from
	 * CONFIG_OSCORE_SERVER_IP and OSCORE keys from the provisioned PSA
	 * context. Pass a struct oscore_cloud_config here to override at runtime. */
	err = oscore_cloud_init(NULL);
	if (err) {
		LOG_ERR("oscore_cloud_init failed: %d", err);
		return err;
	}

	while (1) {
		/* Never touch the cloud transport while the radio link is down; wait
		 * for it and escalate recovery (which may reinit the modem, so the
		 * socket must be closed first) if it stays down. */
		if (!network_wait_connected(K_SECONDS(CONFIG_APP_LINK_TIMEOUT_SECONDS))) {
			connected = false;
			oscore_cloud_disconnect();
			network_recover(++failures);
			continue;
		}

		if (!connected) {
			err = oscore_cloud_connect();
			if (err) {
				/* A link drop during connect is a network problem; anything
				 * else is a transport error we retry with the library's
				 * backoff. Close the socket before any modem-level recovery. */
				if (!network_is_connected()) {
					oscore_cloud_disconnect();
					network_recover(++failures);
					continue;
				}

				uint32_t backoff = oscore_cloud_backoff_seconds(++attempts);

				LOG_WRN("Connect failed (%d); retrying in %u s (attempt %u)",
					err, backoff, attempts);
				k_sleep(K_SECONDS(backoff));
				continue;
			}

			LOG_INF("OSCORE cloud connected");
			attempts = 0;
			failures = 0;
			connected = true;

			/* Refresh the wall clock after (re)connecting; a long outage
			 * may have left the previous time stale or unset. */
			ensure_time();
		}

		struct cloud_telemetry t;

		build_telemetry(&t);

		err = oscore_cloud_send_telemetry(&t, on_shadow_delta, NULL);
		if (err) {
			LOG_ERR("Telemetry send failed (%d); reconnecting", err);
			oscore_cloud_disconnect();
			connected = false;

			/* If the send failed because the link dropped, recover it
			 * before the next iteration; otherwise loop straight back to
			 * a reconnect attempt. */
			if (!network_is_connected()) {
				network_recover(++failures);
			}
			continue;
		}

		LOG_INF("Telemetry sent; next check-in in %u s", interval_s);
		attempts = 0;
		failures = 0;
		k_sleep(K_SECONDS(interval_s));
	}

	return 0;
}
