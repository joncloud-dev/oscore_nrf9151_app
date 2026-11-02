/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GNSS location feature for the OSCORE nRF9151 sample.
 *
 * Framework-free take on the Asset-Tracker-Template location module: it drives
 * the nRF Connect SDK Location library (modem/location.h) with a GNSS-only
 * method and surfaces two things to the plain main() loop:
 *
 *   1. A-GNSS assistance requests raised by the modem (LOCATION_EVT_GNSS_-
 *      ASSISTANCE_REQUEST). The event handler runs on the Location library
 *      workqueue, but the assistance fetch is an OSCORE/CoAP round-trip that
 *      must be serialized with telemetry on the single cloud socket, so the
 *      request is handed to main() which performs the fetch when connected.
 *
 *   2. Completed GNSS fixes, so main() can fold them into a telemetry uplink.
 *
 * Repeat-request handling: because the server returns only a subset of the
 * constellation (pruned / capped), the GNSS core keeps re-raising the
 * assistance request for satellites it can see but was not given. We fetch at
 * most once per search; further requests in the same search are ignored and the
 * modem proceeds to a fix with the data it has (assistance is optional).
 */

#ifndef APP_LOCATION_H_
#define APP_LOCATION_H_

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A completed GNSS fix, ready to be folded into telemetry (agnss-schema.md 2.1). */
struct app_fix {
	double lat;         /* degrees */
	double lon;         /* degrees */
	double accuracy_m;  /* horizontal accuracy, metres */
	double alt_m;       /* altitude, metres (valid only if has_alt) */
	bool has_alt;
	uint32_t ttff_ms;   /* GNSS execution time, ms */
	int64_t timestamp;  /* unix seconds, or 0 if wall clock unknown */
};

/* Initialise the Location library and this module. Call once after the modem
 * library is up (network_init has returned). Returns 0 or a negative errno. */
int app_location_init(void);

/* Which assistance source feeds a GNSS search. The mode decides how the modem's
 * assistance-need event is served: over the network (A-GNSS), from the stored
 * predicted windows (P-GPS), or not at all. */
enum app_assist {
	/* Live A-GNSS fetch, look-ahead 0: only satellites visible now, capped to
	 * CONFIG_APP_AGNSS_SINGLE_SHOT_MAX_SV. Smallest payload per fix. */
	APP_ASSIST_AGNSS_SINGLE,
	/* Live A-GNSS fetch with the full 2 h look-ahead window. */
	APP_ASSIST_AGNSS_WINDOW,
	/* Inject the stored predicted window covering now; never goes online. */
	APP_ASSIST_PGPS,
	/* Serve nothing: the modem fixes on whatever it can decode from the sky.
	 * The control case for assistance comparisons. */
	APP_ASSIST_NONE,
};

/* Human-readable name of an assistance mode, for logs and shell output. */
const char *app_assist_name(enum app_assist mode);

/* Trigger a one-shot GNSS search served by the given assistance mode. all
 * requests the full constellation for the fetch (server skips SV pruning),
 * trading payload size for coverage; it is only meaningful for the A-GNSS modes.
 * Returns 0 or a negative errno. */
int app_location_request_assist(enum app_assist mode, bool all);

/* True while a GNSS search started here is still running. */
bool app_location_search_active(void);

/* Convenience wrapper: APP_ASSIST_AGNSS_SINGLE / APP_ASSIST_AGNSS_WINDOW. */
int app_location_request(bool single_shot, bool all);

/* Convenience wrapper: APP_ASSIST_PGPS. */
int app_location_request_pgps(void);

/* If an A-GNSS assistance request is pending (and not yet fetched for the
 * current search), copy the modem's request frame into *frame, report the
 * search's single_shot mode in *single_shot and its all (unpruned) mode in *all,
 * and return true. Any of frame/single_shot/all may be NULL. The pending flag is
 * cleared; call app_location_agnss_mark_fetched() after a successful fetch so
 * repeat requests in the same search are suppressed. */
bool app_location_agnss_pending(struct nrf_modem_gnss_agnss_data_frame *frame,
				bool *single_shot, bool *all);

/* If the current search was started with app_location_request_pgps() and the
 * modem has raised an assistance-need not yet serviced this search, copy the
 * request frame into *frame (may be NULL) and return true, clearing the pending
 * flag. Call app_location_agnss_mark_fetched() after injecting so repeat
 * requests in this search are suppressed. */
bool app_location_pgps_pending(struct nrf_modem_gnss_agnss_data_frame *frame);

/* Mark that assistance was fetched (A-GNSS) or injected (P-GPS) for the current
 * search, so subsequent LOCATION_EVT_GNSS_ASSISTANCE_REQUEST events in this
 * search are ignored. */
void app_location_agnss_mark_fetched(void);

/* If a new GNSS fix is ready, copy it into *fix and return true (clearing it). */
bool app_location_get_fix(struct app_fix *fix);

/* Number of fixes stored since boot. Lets a caller decide whether a search it
 * started produced a fix without consuming the pending one (which belongs to
 * the telemetry path, and which is not consumed at all while the link is down). */
uint32_t app_location_fix_count(void);

/* Last known GNSS coordinates, for use as an A-GNSS position hint. Returns true
 * if a fix has ever been obtained this power cycle. */
bool app_location_last_coords(double *lat, double *lon, uint32_t *unc_m);

/* Seed the last-known GNSS coordinates manually (degrees, uncertainty in metres),
 * as if a fix had been obtained. Lets a device with no fix this power cycle still
 * provide a coarse reference position for the A-GNSS / P-GPS LOCATION assistance
 * and for server-side per-window pruning. unc_m is clamped to a minimum of 1 m. */
void app_location_set_last_coords(double lat, double lon, uint32_t unc_m);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOCATION_H_ */
