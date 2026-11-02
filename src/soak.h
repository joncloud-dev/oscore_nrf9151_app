/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Long-running assistance soak test.
 *
 * Repeatedly acquires a GNSS fix and uplinks it, once every N minutes for hours
 * or days, with the assistance source pinned for the whole run. That makes the
 * measurement pgps-implementation-plan.md section 8.4 asks for: hold a unit with
 * a stored horizon and fix from it every few hours, recording TTFF against how
 * old the prediction was. Run two DKs side by side -- one on `pgps`, one on
 * `agnss` -- and the two paths see the same sky, the same sky-view and the same
 * hardware.
 *
 * The pinned mode is never silently swapped: if the chosen assistance is
 * unavailable (no stored window covers now, or no link for an A-GNSS fetch) the
 * cycle fixes unassisted and reports that it did, so a fallback shows up in the
 * data instead of quietly contaminating it.
 *
 * State is RAM-only: a reboot ends the run (the stored P-GPS windows survive, so
 * `soak start` simply picks up where it left off). Everything runs from the
 * main() loop via soak_tick(), like the rest of this sample.
 */

#ifndef APP_SOAK_H_
#define APP_SOAK_H_

#include <stdbool.h>
#include <stdint.h>

#include "location.h"

struct cloud_telemetry;

/* Start a run: one fix every interval_min minutes, served by mode. duration_h
 * of 0 runs until soak_stop(). all is passed to the A-GNSS fetch (full
 * constellation). Returns 0, -EALREADY if a run is active, or -EINVAL. */
int soak_start(enum app_assist mode, uint32_t interval_min, uint32_t duration_h, bool all);

/* Stop the current run (no-op if none is active). */
void soak_stop(void);

/* Drive the run. Call once per second from the main loop. */
void soak_tick(void);

/* Record whether the cycle's intended assistance was actually applied. Called
 * from the main loop right after servicing an A-GNSS fetch or a P-GPS
 * injection. */
void soak_note_assist(bool applied);

/* Record the fix that just completed, for the TTFF statistics. */
void soak_note_fix(uint32_t ttff_ms);

/* Fold this cycle's soak metrics into an outgoing telemetry record. No-op when
 * no run is active. */
void soak_add_metrics(struct cloud_telemetry *t);

/* True while a run is active. */
bool soak_running(void);

#endif /* APP_SOAK_H_ */
