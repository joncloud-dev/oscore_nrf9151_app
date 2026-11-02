/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Long-running assistance soak test. See soak.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <oscore_cloud/cloud_client.h>
#ifdef CONFIG_OSCORE_CLOUD_PGPS
#include <oscore_cloud/pgps_client.h>
#endif
#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
#include <oscore_cloud/pgps_store.h>
#endif

#include "soak.h"

LOG_MODULE_REGISTER(soak, LOG_LEVEL_INF);

/* Free-form telemetry metric keys for the soak run. The server stores any
 * non-well-known key as a metric, so these join against pgps_windows on
 * gen/toe and give the horizon-decay plot its x axis (key 32). */
#define SOAK_KEY_PATH      30 /* assistance actually used: 0 none, 1 A-GNSS, 2 P-GPS */
#define SOAK_KEY_CYCLE     31 /* cycle index within the run */
#define SOAK_KEY_PRED_AGE  32 /* age of the injected prediction, minutes */
#define SOAK_KEY_SV        33 /* ephemerides injected */
#define SOAK_KEY_ASSISTED  34 /* 1 = the pinned assistance was applied, 0 = fell back */

#define SOAK_PATH_NONE  0
#define SOAK_PATH_AGNSS 1
#define SOAK_PATH_PGPS  2

static struct {
	bool running;
	enum app_assist mode;
	bool all;
	uint32_t interval_s;
	int64_t next_due_ms;
	int64_t end_ms; /* 0 = no deadline */

	/* Run totals. */
	uint32_t cycles;    /* cycles started */
	uint32_t fixes;     /* cycles that produced a fix */
	uint32_t misses;    /* cycles that ended without a fix */
	uint32_t fallbacks; /* cycles that ran without the pinned assistance */
	uint64_t ttff_sum_ms;
	uint32_t last_ttff_ms;

	/* Current cycle. */
	bool cycle_active;
	uint32_t cycle_fix_base; /* app_location_fix_count() when the cycle started */
	bool cycle_assisted;
	uint32_t cycle_pred_age_min;
	uint32_t cycle_ttff_ms; /* 0 until this cycle's fix is reported */
	uint8_t cycle_sv;
} soak;

static uint8_t path_code(enum app_assist mode)
{
	switch (mode) {
	case APP_ASSIST_AGNSS_SINGLE:
	case APP_ASSIST_AGNSS_WINDOW:
		return SOAK_PATH_AGNSS;
	case APP_ASSIST_PGPS:
		return SOAK_PATH_PGPS;
	default:
		return SOAK_PATH_NONE;
	}
}

int soak_start(enum app_assist mode, uint32_t interval_min, uint32_t duration_h, bool all)
{
	if (soak.running) {
		return -EALREADY;
	}
	if (interval_min == 0) {
		return -EINVAL;
	}

	memset(&soak, 0, sizeof(soak));
	soak.running = true;
	soak.mode = mode;
	soak.all = all;
	soak.interval_s = interval_min * 60U;
	/* Fire the first cycle immediately so the run is visibly alive rather than
	 * silent for the first interval. */
	soak.next_due_ms = k_uptime_get();
	if (duration_h) {
		soak.end_ms = k_uptime_get() + (int64_t)duration_h * 3600 * MSEC_PER_SEC;
	}

	LOG_INF("Soak started: %s, one fix every %u min%s%s", app_assist_name(mode), interval_min,
		duration_h ? ", for " : " (until `soak stop`)", all ? ", all SVs" : "");
	if (duration_h) {
		LOG_INF("Soak deadline: %u h from now", duration_h);
	}
	return 0;
}

static void log_summary(const char *why)
{
	uint32_t mean = soak.fixes ? (uint32_t)(soak.ttff_sum_ms / soak.fixes) : 0;

	LOG_INF("Soak %s after %u cycle(s): %u fix, %u miss, %u ran without the pinned "
		"assistance, mean TTFF %u ms",
		why, soak.cycles, soak.fixes, soak.misses, soak.fallbacks, mean);
}

void soak_stop(void)
{
	if (!soak.running) {
		return;
	}
	log_summary("stopped");
	soak.running = false;
	soak.cycle_active = false;
}

bool soak_running(void)
{
	return soak.running;
}

void soak_note_assist(bool applied)
{
	if (!soak.running || !soak.cycle_active) {
		return;
	}

	soak.cycle_assisted = applied;
	if (!applied) {
		LOG_WRN("Soak cycle %u: %s unavailable; fixing unassisted", soak.cycles,
			app_assist_name(soak.mode));
	}

#ifdef CONFIG_OSCORE_CLOUD_PGPS
	if (applied && soak.mode == APP_ASSIST_PGPS) {
		struct pgps_inject_info info;

		if (pgps_client_last_inject(&info) == 0) {
			soak.cycle_pred_age_min = info.gen_age_s / 60U;
			soak.cycle_sv = info.sv_count;
		}
	}
#endif
}

void soak_note_fix(uint32_t ttff_ms)
{
	/* Gated on the cycle so a manual `loc single` in the middle of a run does
	 * not land in the run's statistics. */
	if (!soak.running || !soak.cycle_active) {
		return;
	}
	soak.ttff_sum_ms += ttff_ms;
	soak.last_ttff_ms = ttff_ms;
	soak.cycle_ttff_ms = ttff_ms;
}

void soak_add_metrics(struct cloud_telemetry *t)
{
	if (!soak.running || !soak.cycle_active) {
		return;
	}

	(void)cloud_telemetry_add_uint(t, SOAK_KEY_PATH,
				       soak.cycle_assisted ? path_code(soak.mode) : SOAK_PATH_NONE);
	(void)cloud_telemetry_add_uint(t, SOAK_KEY_CYCLE, soak.cycles);
	(void)cloud_telemetry_add_uint(t, SOAK_KEY_PRED_AGE, soak.cycle_pred_age_min);
	(void)cloud_telemetry_add_uint(t, SOAK_KEY_SV, soak.cycle_sv);
	(void)cloud_telemetry_add_uint(t, SOAK_KEY_ASSISTED, soak.cycle_assisted ? 1 : 0);
}

static void cycle_start(void)
{
	int err;

	soak.cycles++;
	soak.cycle_fix_base = app_location_fix_count();
	/* Nothing to apply for the unassisted control, so it never counts as a
	 * fallback. */
	soak.cycle_assisted = (soak.mode == APP_ASSIST_NONE);
	soak.cycle_pred_age_min = 0;
	soak.cycle_ttff_ms = 0;
	soak.cycle_sv = 0;

	/* Marked active before the request: the Location library raises the
	 * assistance-need on its own workqueue, and soak_note_assist() must not
	 * be ignored if the main loop services it promptly. */
	soak.cycle_active = true;

	err = app_location_request_assist(soak.mode, soak.all);
	if (err) {
		LOG_WRN("Soak cycle %u: search request failed: %d", soak.cycles, err);
		soak.cycle_active = false;
		soak.misses++;
		return;
	}
	LOG_INF("Soak cycle %u started (%s)", soak.cycles, app_assist_name(soak.mode));
}

void soak_tick(void)
{
	int64_t now;

	if (!soak.running) {
		return;
	}

	now = k_uptime_get();

	/* A cycle ends when the search does; whether it produced a fix is read
	 * from the fix counter rather than the pending fix, which belongs to the
	 * telemetry path and is not consumed at all while the link is down. */
	if (soak.cycle_active && !app_location_search_active()) {
		soak.cycle_active = false;
		/* Counted here rather than in soak_note_assist() so that a cycle
		 * whose assistance never got served at all -- no link for the
		 * A-GNSS fetch, say -- is recorded as unassisted too. */
		if (!soak.cycle_assisted) {
			soak.fallbacks++;
		}
		if (app_location_fix_count() != soak.cycle_fix_base) {
			soak.fixes++;
			/* TTFF comes in through the telemetry path, which is skipped
			 * while the link is down -- so it can legitimately be
			 * missing for a cycle that did fix. */
			if (soak.cycle_ttff_ms) {
				LOG_INF("Soak cycle %u: fix in %u ms (%s)", soak.cycles,
					soak.cycle_ttff_ms,
					soak.cycle_assisted ? "assisted" : "unassisted");
			} else {
				LOG_INF("Soak cycle %u: fix, TTFF not reported (%s)", soak.cycles,
					soak.cycle_assisted ? "assisted" : "unassisted");
			}
		} else {
			soak.misses++;
			LOG_WRN("Soak cycle %u: no fix", soak.cycles);
		}
	}

	if (soak.end_ms && now >= soak.end_ms && !soak.cycle_active) {
		log_summary("finished");
		soak.running = false;
		return;
	}

	if (soak.cycle_active || now < soak.next_due_ms) {
		return;
	}

	/* Schedule from the cycle grid, not from completion, so the cadence does
	 * not drift by the search duration; skip ahead if a cycle overran its
	 * slot rather than firing a burst to catch up. */
	do {
		soak.next_due_ms += (int64_t)soak.interval_s * MSEC_PER_SEC;
	} while (soak.next_due_ms <= now);

	if (soak.end_ms && now >= soak.end_ms) {
		log_summary("finished");
		soak.running = false;
		return;
	}

	cycle_start();
}

/* ---- shell -------------------------------------------------------------- */

static int parse_mode(const char *s, enum app_assist *mode)
{
	if (!strcmp(s, "pgps")) {
		*mode = APP_ASSIST_PGPS;
	} else if (!strcmp(s, "agnss")) {
		*mode = APP_ASSIST_AGNSS_SINGLE;
	} else if (!strcmp(s, "agnss-window")) {
		*mode = APP_ASSIST_AGNSS_WINDOW;
	} else if (!strcmp(s, "none")) {
		*mode = APP_ASSIST_NONE;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int cmd_soak_start(const struct shell *sh, size_t argc, char **argv)
{
	enum app_assist mode;
	unsigned long interval_min;
	unsigned long duration_h = 0;
	bool all = false;
	char *end;
	int err;

	if (parse_mode(argv[1], &mode)) {
		shell_error(sh, "unknown mode '%s' (pgps|agnss|agnss-window|none)", argv[1]);
		return -EINVAL;
	}

	interval_min = strtoul(argv[2], &end, 10);
	if (*end != '\0' || interval_min == 0) {
		shell_error(sh, "invalid interval '%s' (expected minutes > 0)", argv[2]);
		return -EINVAL;
	}

	for (size_t i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "all")) {
			all = true;
			continue;
		}
		duration_h = strtoul(argv[i], &end, 10);
		if (*end != '\0' || duration_h == 0) {
			shell_error(sh, "invalid duration '%s' (expected hours > 0, or \"all\")",
				    argv[i]);
			return -EINVAL;
		}
	}

	err = soak_start(mode, (uint32_t)interval_min, (uint32_t)duration_h, all);
	if (err == -EALREADY) {
		shell_error(sh, "a soak run is already active; `soak stop` it first");
		return err;
	}
	if (err) {
		shell_error(sh, "soak start failed: %d", err);
		return err;
	}

	shell_print(sh, "Soak started: %s, one fix every %lu min%s", app_assist_name(mode),
		    interval_min, all ? ", all SVs" : "");
	if (duration_h) {
		shell_print(sh, "Run ends in %lu h", duration_h);
	} else {
		shell_print(sh, "Runs until `soak stop`");
	}
	return 0;
}

static int cmd_soak_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!soak_running()) {
		shell_print(sh, "No soak run active");
		return 0;
	}
	soak_stop();
	shell_print(sh, "Soak stopped");
	return 0;
}

static int cmd_soak_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!soak.running) {
		shell_print(sh, "Soak idle");
	} else {
		int64_t left_ms = soak.next_due_ms - k_uptime_get();

		shell_print(sh, "Soak running: %s, every %u min, next cycle in %d s",
			    app_assist_name(soak.mode), soak.interval_s / 60U,
			    (int)(left_ms > 0 ? left_ms / MSEC_PER_SEC : 0));
		if (soak.end_ms) {
			shell_print(sh, "  ends in %d min",
				    (int)((soak.end_ms - k_uptime_get()) / (60 * MSEC_PER_SEC)));
		}
	}

	shell_print(sh, "  cycles %u: %u fix, %u miss, %u without the pinned assistance",
		    soak.cycles, soak.fixes, soak.misses, soak.fallbacks);
	if (soak.fixes) {
		shell_print(sh, "  TTFF: last %u ms, mean %u ms", soak.last_ttff_ms,
			    (uint32_t)(soak.ttff_sum_ms / soak.fixes));
	}

#ifdef CONFIG_OSCORE_CLOUD_PGPS_STORE
	struct pgps_store_stats st;

	pgps_store_get_stats(&st);
	shell_print(sh, "  P-GPS store: %u/%u windows, toe %u..%u, newest gen %u", st.used,
		    st.slots, st.first_toe, st.last_toe, st.newest_gen);
#endif
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(soak_cmds,
	SHELL_CMD_ARG(start, NULL,
		      "Start a run: start <pgps|agnss|agnss-window|none> <interval_min> "
		      "[duration_h] [all]",
		      cmd_soak_start, 3, 2),
	SHELL_CMD(stop, NULL, "Stop the current run", cmd_soak_stop),
	SHELL_CMD(status, NULL, "Show run progress and the stored P-GPS horizon",
		  cmd_soak_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(soak, &soak_cmds, "Long-running assistance soak test", NULL);
