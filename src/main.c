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
#include <string.h>

#include <date_time.h>

#include <oscore_cloud/cloud_config.h>
#include <oscore_cloud/cloud_session.h>
#include <oscore_cloud/cloud_client.h>
#ifdef CONFIG_OSCORE_CLOUD_DIAG
#include <oscore_cloud/diag_bridge.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_AGNSS
#include <oscore_cloud/agnss_client.h>
#endif

#include "network.h"
#ifdef CONFIG_LOCATION
#include "location.h"
#include <zephyr/shell/shell.h>
#endif

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

#ifdef CONFIG_LOCATION
		/* Cloud-driven location trigger: desired {"loc":"single"|"window"}
		 * kicks off a GNSS fix with the matching A-GNSS mode. */
		if (strcmp(delta->entries[i].key, "loc") == 0) {
			if (strstr(delta->entries[i].value, "single")) {
				(void)app_location_request(true);
			} else if (strstr(delta->entries[i].value, "window")) {
				(void)app_location_request(false);
			}
		}
#endif
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

#ifdef CONFIG_LOCATION

#ifdef CONFIG_OSCORE_CLOUD_AGNSS
/* Build the A-GNSS fetch options for the current search: the look-ahead window
 * and SV cap follow the single-shot vs windowed choice, and the position hint is
 * the last GNSS fix if we have one, else the serving cell (agnss-schema.md 1.1). */
static void build_agnss_opts(struct agnss_fetch_opts *o, bool single_shot)
{
	double lat, lon;
	uint32_t unc;

	memset(o, 0, sizeof(*o));
	o->encoding = IS_ENABLED(CONFIG_OSCORE_CLOUD_AGNSS_COMPACT) ? 1 : 0;
	if (single_shot) {
		o->window_s = 0; /* only satellites visible now */
		o->max_sv = CONFIG_APP_AGNSS_SINGLE_SHOT_MAX_SV;
	} else {
		o->window_s = CONFIG_OSCORE_CLOUD_AGNSS_WINDOW_S_DEFAULT;
		o->max_sv = 0;
	}

	if (app_location_last_coords(&lat, &lon, &unc)) {
		o->have_pos = true;
		o->lat = lat;
		o->lon = lon;
		o->unc_m = unc;
	} else {
		struct network_cell c;

		if (network_serving_cell(&c)) {
			o->have_cell = true;
			o->mcc = c.mcc;
			o->mnc = c.mnc;
			o->tac = c.tac;
			o->ci = c.id;
			o->rsrp = c.rsrp;
			o->ta = c.ta;
		}
	}
}

/* Serve a pending A-GNSS assistance request over the (connected) cloud socket.
 * Called from the main loop only, so it is serialized with telemetry sends. */
static void service_agnss(void)
{
	struct nrf_modem_gnss_agnss_data_frame frame;
	bool single_shot;

	if (!network_is_connected()) {
		return;
	}
	if (!app_location_agnss_pending(&frame, &single_shot)) {
		return;
	}

	struct agnss_fetch_opts opts;

	build_agnss_opts(&opts, single_shot);

	int err = agnss_client_fetch_ex(&frame, &opts);

	if (err) {
		LOG_WRN("A-GNSS fetch failed: %d", err);
	} else {
		app_location_agnss_mark_fetched();
	}
}
#endif /* CONFIG_OSCORE_CLOUD_AGNSS */

/* Fold a completed GNSS fix into a telemetry uplink (key 4) and send it. */
static void service_fix(void)
{
	struct app_fix f;

	if (!network_is_connected() || !app_location_get_fix(&f)) {
		return;
	}

	struct cloud_telemetry t;

	build_telemetry(&t);

	struct cloud_telemetry_gnss g = {
		.valid = true,
		.lat = f.lat,
		.lon = f.lon,
		.accuracy = f.accuracy_m,
		.have_alt = f.has_alt,
		.altitude = f.alt_m,
		.ttff_ms = f.ttff_ms,
		.fix_time_sec = (uint64_t)f.timestamp,
	};

	cloud_telemetry_set_gnss(&t, &g);

	int err = oscore_cloud_send_telemetry(&t, on_shadow_delta, NULL);

	if (err) {
		LOG_WRN("Fix telemetry send failed: %d (will retry on next check-in)", err);
	} else {
		LOG_INF("GNSS fix reported (ttff %u ms)", f.ttff_ms);
	}
}

/* Poll location work (A-GNSS fetch + fix reporting) for up to secs, in 1 s
 * steps, so assistance requests raised by an in-flight search are served
 * promptly instead of waiting for the next check-in. */
static void location_wait(uint32_t secs)
{
	for (uint32_t s = 0; s < secs; s++) {
#ifdef CONFIG_OSCORE_CLOUD_AGNSS
		service_agnss();
#endif
		service_fix();
		k_sleep(K_SECONDS(1));
	}
}

static int cmd_loc_single(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Triggering single-shot GNSS fix");
	return app_location_request(true);
}

static int cmd_loc_window(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Triggering 2h-window GNSS fix");
	return app_location_request(false);
}

SHELL_STATIC_SUBCMD_SET_CREATE(loc_cmds,
	SHELL_CMD(single, NULL, "GNSS fix with single-shot A-GNSS (smallest payload)", cmd_loc_single),
	SHELL_CMD(window, NULL, "GNSS fix with 2h-window A-GNSS", cmd_loc_window),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(loc, &loc_cmds, "GNSS location commands", NULL);

#else /* !CONFIG_LOCATION */

static void location_wait(uint32_t secs) { k_sleep(K_SECONDS(secs)); }

#endif /* CONFIG_LOCATION */

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

#ifdef CONFIG_LOCATION
	err = app_location_init();
	if (err) {
		LOG_WRN("Location init failed (%d); GNSS features disabled", err);
	}
#endif

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

		/* Poll location work during the idle interval so A-GNSS requests from
		 * an in-flight GNSS search are served and fixes reported promptly. */
		location_wait(interval_s);
	}

	return 0;
}
