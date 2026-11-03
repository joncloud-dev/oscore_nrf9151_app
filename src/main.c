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
#include <stdlib.h>
#include <errno.h>

#include <date_time.h>

#include <oscore_cloud/cloud_config.h>
#include <oscore_cloud/cloud_session.h>
#include <oscore_cloud/cloud_client.h>
#ifdef CONFIG_OSCORE_CLOUD_FOTA
#include <oscore_cloud/fota_client.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_DIAG
#include <oscore_cloud/diag_bridge.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_AGNSS
#include <oscore_cloud/agnss_client.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_PGPS
#include <oscore_cloud/pgps_client.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
#include <oscore_cloud/pgps_store.h>
#endif

#include "network.h"
#ifdef CONFIG_LOCATION
#include "location.h"
#include "soak.h"
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
		 * kicks off a GNSS fix with the matching A-GNSS mode. Append "all"
		 * (e.g. "single all") to request the full, unpruned constellation. */
		if (strcmp(delta->entries[i].key, "loc") == 0) {
			bool all = strstr(delta->entries[i].value, "all") != NULL;

			if (strstr(delta->entries[i].value, "single")) {
				(void)app_location_request(true, all);
			} else if (strstr(delta->entries[i].value, "window")) {
				(void)app_location_request(false, all);
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
static void build_agnss_opts(struct agnss_fetch_opts *o, bool single_shot, bool all)
{
	double lat, lon;
	uint32_t unc;

	memset(o, 0, sizeof(*o));
	o->encoding = IS_ENABLED(CONFIG_OSCORE_CLOUD_AGNSS_COMPACT) ? 1 : 0;
	o->all = all;
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
	bool all;

	if (!network_is_connected()) {
		return;
	}
	if (!app_location_agnss_pending(&frame, &single_shot, &all)) {
		return;
	}

	struct agnss_fetch_opts opts;

	build_agnss_opts(&opts, single_shot, all);

	int err = agnss_client_fetch_ex(&frame, &opts);

	if (err) {
		LOG_WRN("A-GNSS fetch failed: %d", err);
	} else {
		app_location_agnss_mark_fetched();
	}
	soak_note_assist(err == 0);
}
#endif /* CONFIG_OSCORE_CLOUD_AGNSS */

#ifdef CONFIG_OSCORE_CLOUD_PGPS
/* Set by the `pgps fetch` shell command; serviced from the main loop when the
 * cloud link is up (the Block2 POST must be serialized with telemetry on the
 * single socket, like the A-GNSS fetch). */
static atomic_t pgps_fetch_req;   /* 1 = a fetch is queued */
static uint16_t pgps_fetch_n;     /* window count for the queued fetch (0 = default) */
static bool pgps_fetch_all;       /* request the full constellation (skip pruning) */
static bool pgps_fetch_running;   /* a batched fetch is in progress */

/* Fill P-GPS options with the last-known GNSS position as a coarse hint (used
 * both for server-side per-window pruning and as the injected reference
 * location). The /p request has no serving-cell hint, so we send coordinates
 * only. */
static void build_pgps_opts(struct pgps_fetch_opts *o, uint16_t n, bool all)
{
	double lat, lon;
	uint32_t unc;

	memset(o, 0, sizeof(*o));
	o->n = n;
	o->all = all;
	if (app_location_last_coords(&lat, &lon, &unc)) {
		o->have_pos = true;
		o->lat = lat;
		o->lon = lon;
		o->unc_m = unc;
	}
}

/* Service P-GPS work from the main loop: perform a queued window fetch when the
 * link is up, and inject the stored covering window when a P-GPS-mode search
 * raises the modem's assistance-need. Serialized with telemetry sends. */
static void service_pgps(void)
{
	if (atomic_cas(&pgps_fetch_req, 1, 0)) {
		if (network_is_connected()) {
			struct pgps_fetch_opts opts;

			build_pgps_opts(&opts, pgps_fetch_n, pgps_fetch_all);

			int err = pgps_client_fetch_begin(&opts, pgps_fetch_n);

			if (err) {
				LOG_WRN("P-GPS fetch could not start: %d", err);
			} else {
				pgps_fetch_running = true;
			}
		} else {
			/* Not connected yet: re-queue and try on a later iteration. */
			atomic_set(&pgps_fetch_req, 1);
		}
	}

	/* One /p request per iteration. A long horizon needs many of them, and
	 * pumping one at a time keeps telemetry and the GNSS search serviced in
	 * between instead of blocking the loop for the whole run. */
	if (pgps_fetch_running) {
		if (pgps_client_step() != PGPS_STEP_MORE) {
			uint16_t stored, remaining;
			int err;

			pgps_fetch_running = false;
			pgps_client_fetch_status(&stored, &remaining, &err);
			LOG_INF("P-GPS fetch complete: %u window(s) stored%s%s", stored,
				remaining ? " (horizon short of the request)" : "",
				pgps_client_have_window_now() ? ", a window covers now"
							      : ", no window covers now yet");
			if (err) {
				LOG_WRN("P-GPS fetch ended on error %d", err);
			}
		}
	}

	if (app_location_pgps_pending(NULL)) {
		struct pgps_fetch_opts opts;

		/* Inject uses only the position hint from opts; `all` (a fetch-time
		 * pruning control) is irrelevant here. */
		build_pgps_opts(&opts, 0, false);

		int err = pgps_client_inject_now(&opts);

		if (err) {
			LOG_WRN("P-GPS inject failed: %d (fix will proceed without it)", err);
		}
		/* Mark serviced either way so the search does not re-raise into a loop. */
		app_location_agnss_mark_fetched();
		soak_note_assist(err == 0);
	}
}
#endif /* CONFIG_OSCORE_CLOUD_PGPS */

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

	/* During a soak run, tag the uplink with which assistance path actually
	 * fed this fix and how stale the prediction was, so the backend can plot
	 * TTFF against horizon age. No-op when no run is active. */
	soak_note_fix(f.ttff_ms);
	soak_add_metrics(&t);

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
#ifdef CONFIG_OSCORE_CLOUD_PGPS
		service_pgps();
#endif
		service_fix();
		soak_tick();
		k_sleep(K_SECONDS(1));
	}
}

/* Optional trailing "all" token on a loc subcommand: request the full
 * constellation (server skips SV pruning) for this fix. */
static bool loc_all_arg(size_t argc, char **argv)
{
	return argc > 1 && strcmp(argv[1], "all") == 0;
}

static int cmd_loc_single(const struct shell *sh, size_t argc, char **argv)
{
	bool all = loc_all_arg(argc, argv);

	shell_print(sh, "Triggering single-shot GNSS fix%s", all ? " (all SVs)" : "");
	return app_location_request(true, all);
}

static int cmd_loc_window(const struct shell *sh, size_t argc, char **argv)
{
	bool all = loc_all_arg(argc, argv);

	shell_print(sh, "Triggering 2h-window GNSS fix%s", all ? " (all SVs)" : "");
	return app_location_request(false, all);
}

/* Seed (or show) the last-known position. With no args, print the current stored
 * coordinates; with <lat> <lon> [unc_m] set them. This is the reference position
 * injected as NRF_MODEM_GNSS_AGNSS_LOCATION ahead of the P-GPS ephemerides (and
 * used as the A-GNSS position hint / server pruning anchor), letting a device
 * with no fix this power cycle still constrain the Doppler/visibility search. */
static int cmd_loc_setpos(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		double lat, lon;
		uint32_t unc;

		if (app_location_last_coords(&lat, &lon, &unc)) {
			shell_print(sh, "Last-known position: %.06f, %.06f (unc %u m)",
				    lat, lon, unc);
		} else {
			shell_print(sh, "No last-known position stored this power cycle");
		}
		return 0;
	}

	if (argc < 3) {
		shell_error(sh, "usage: loc setpos <lat> <lon> [unc_m]");
		return -EINVAL;
	}

	char *end;
	double lat = strtod(argv[1], &end);

	if (*end != '\0' || lat < -90.0 || lat > 90.0) {
		shell_error(sh, "invalid latitude '%s' (expected -90..90)", argv[1]);
		return -EINVAL;
	}

	double lon = strtod(argv[2], &end);

	if (*end != '\0' || lon < -180.0 || lon > 180.0) {
		shell_error(sh, "invalid longitude '%s' (expected -180..180)", argv[2]);
		return -EINVAL;
	}

	uint32_t unc = 100; /* metres; coarse default when the caller omits it */

	if (argc >= 4) {
		unsigned long v = strtoul(argv[3], &end, 10);

		if (*end != '\0' || v == 0) {
			shell_error(sh, "invalid uncertainty '%s' (expected metres > 0)", argv[3]);
			return -EINVAL;
		}
		unc = (uint32_t)v;
	}

	app_location_set_last_coords(lat, lon, unc);
	shell_print(sh, "Last-known position set: %.06f, %.06f (unc %u m)", lat, lon, unc);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(loc_cmds,
	SHELL_CMD_ARG(single, NULL,
		      "GNSS fix with single-shot A-GNSS (smallest payload) [all]",
		      cmd_loc_single, 1, 1),
	SHELL_CMD_ARG(window, NULL, "GNSS fix with 2h-window A-GNSS [all]",
		      cmd_loc_window, 1, 1),
	SHELL_CMD_ARG(setpos, NULL,
		      "Set/show last-known position hint: setpos <lat> <lon> [unc_m]",
		      cmd_loc_setpos, 1, 4),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(loc, &loc_cmds, "GNSS location commands", NULL);

#ifdef CONFIG_OSCORE_CLOUD_PGPS
/* How many windows a single fetch can usefully ask for: the ring is the limit,
 * since anything beyond it would overwrite the windows fetched earlier in the
 * same run. Without the ring the RAM array is the limit. */
#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
#define PGPS_FETCH_MAX CONFIG_OSCORE_CLOUD_PGPS_STORE_SLOTS
#else
#define PGPS_FETCH_MAX CONFIG_OSCORE_CLOUD_PGPS_MAX_WINDOWS
#endif

static int cmd_pgps_fetch(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long n = 0;
	bool all = false;

	/* Args in any order: a decimal window count and/or the literal "all". */
	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "all") == 0) {
			all = true;
		} else {
			n = strtoul(argv[i], NULL, 10);
			if (n > PGPS_FETCH_MAX) {
				shell_warn(sh, "clamping %lu to the %d-window store", n,
					   PGPS_FETCH_MAX);
				n = PGPS_FETCH_MAX;
			}
		}
	}
	pgps_fetch_n = (uint16_t)n;
	pgps_fetch_all = all;
	atomic_set(&pgps_fetch_req, 1);
	if (n) {
		shell_print(sh, "P-GPS fetch (%lu windows%s) queued; the main loop runs it in "
				"batches of %d, one per second (requires the cloud link up)",
			    n, all ? ", all SVs" : "", CONFIG_OSCORE_CLOUD_PGPS_FETCH_BATCH);
	} else {
		shell_print(sh, "P-GPS fetch (default windows%s) queued; the main loop runs it "
				"within ~1 s (requires the cloud link up)",
			    all ? ", all SVs" : "");
	}
	return 0;
}

static int cmd_pgps_fix(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!pgps_client_have_window_now()) {
		shell_warn(sh, "No stored predicted window covers now; run `pgps fetch` first "
			       "(the fix will proceed without predicted assistance)");
	}
	shell_print(sh, "Triggering GNSS fix from stored predicted assistance");
	return app_location_request_pgps();
}

#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
/* Report what the flash ring holds. Coverage is derived from the newest stored
 * window: predictions stay usable until 2 h past their centre. */
static int cmd_pgps_store_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct pgps_store_stats st;

	pgps_store_get_stats(&st);

	shell_print(sh, "P-GPS store: %u/%u window slots used (%u KiB partition)", st.used,
		    st.slots, (unsigned)(st.slots * 4));
	if (st.used == 0) {
		shell_print(sh, "  empty; run `pgps fetch <n>` to download a horizon");
		return 0;
	}
	shell_print(sh, "  window centres (GPS s): %u .. %u", st.first_toe, st.last_toe);
	shell_print(sh, "  span %u h, newest generation %u",
		    (unsigned)((st.last_toe - st.first_toe) / 3600U + 4U), st.newest_gen);
	shell_print(sh, "  a window covers now: %s",
		    pgps_client_have_window_now() ? "yes" : "no");
	return 0;
}

static int cmd_pgps_store_erase(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err = pgps_store_erase_all();

	if (err) {
		shell_error(sh, "erase failed: %d", err);
		return err;
	}
	shell_print(sh, "P-GPS store erased");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pgps_store_cmds,
	SHELL_CMD(erase, NULL, "Wipe the whole window ring", cmd_pgps_store_erase),
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_OSCORE_CLOUD_PGPS_STORE */

SHELL_STATIC_SUBCMD_SET_CREATE(pgps_cmds,
	SHELL_CMD_ARG(fetch, NULL,
		      "Fetch predicted windows from the cloud (POST /p) [n] [all]",
		      cmd_pgps_fetch, 1, 2),
	SHELL_CMD(fix, NULL, "GNSS fix fed by the stored covering predicted window", cmd_pgps_fix),
#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
	SHELL_CMD(store, &pgps_store_cmds, "Show the stored window horizon [erase]",
		  cmd_pgps_store_show),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pgps, &pgps_cmds, "Predicted-GNSS (P-GPS) commands", NULL);
#endif /* CONFIG_OSCORE_CLOUD_PGPS */

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

#ifdef CONFIG_OSCORE_CLOUD_FOTA
	/* After a FOTA swap: confirm the new image (so MCUboot does not revert it)
	 * and latch the SUCCEEDED/FAILED report, which the next FOTA step sends. */
	fota_client_boot_check();
#endif

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

#ifdef CONFIG_OSCORE_CLOUD_PGPS
	/* Opens the predicted-window store and scans what is already in flash, so
	 * a horizon downloaded before the last reboot is usable immediately. */
	err = pgps_client_init();
	if (err) {
		LOG_WRN("P-GPS store init failed (%d); windows will not persist", err);
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
