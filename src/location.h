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

/* Trigger a one-shot GNSS search. single_shot selects the A-GNSS look-ahead
 * window sent with the assistance fetch: true = instant/single-shot (smallest
 * payload, few-minutes validity), false = full 2h window. Returns 0 or errno. */
int app_location_request(bool single_shot);

/* If an A-GNSS assistance request is pending (and not yet fetched for the
 * current search), copy the modem's request frame into *frame, report the
 * search's single_shot mode in *single_shot, and return true. The pending flag
 * is cleared; call app_location_agnss_mark_fetched() after a successful fetch so
 * repeat requests in the same search are suppressed. */
bool app_location_agnss_pending(struct nrf_modem_gnss_agnss_data_frame *frame,
				bool *single_shot);

/* Mark that assistance was fetched for the current search, so subsequent
 * LOCATION_EVT_GNSS_ASSISTANCE_REQUEST events in this search are ignored. */
void app_location_agnss_mark_fetched(void);

/* If a new GNSS fix is ready, copy it into *fix and return true (clearing it). */
bool app_location_get_fix(struct app_fix *fix);

/* Last known GNSS coordinates, for use as an A-GNSS position hint. Returns true
 * if a fix has ever been obtained this power cycle. */
bool app_location_last_coords(double *lat, double *lon, uint32_t *unc_m);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOCATION_H_ */
