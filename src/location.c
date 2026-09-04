/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GNSS location feature. See location.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/location.h>
#include <nrf_modem_gnss.h>
#include <date_time.h>

#include "location.h"

LOG_MODULE_REGISTER(app_location, LOG_LEVEL_INF);

/* Shared state between the Location library workqueue (writer) and main()
 * (reader/consumer). A single mutex keeps the frame copy + flags consistent. */
static K_MUTEX_DEFINE(loc_lock);

static bool search_active;
static bool current_single_shot;

static bool agnss_pending;
static bool agnss_fetched;   /* fetched at least once for the current search */
static struct nrf_modem_gnss_agnss_data_frame agnss_frame;

static bool fix_ready;
static struct app_fix pending_fix;

static bool have_last_coords;
static double last_lat, last_lon;
static uint32_t last_unc_m;

static void search_reset(bool single_shot)
{
	k_mutex_lock(&loc_lock, K_FOREVER);
	search_active = true;
	current_single_shot = single_shot;
	agnss_pending = false;
	agnss_fetched = false;
	k_mutex_unlock(&loc_lock);
}

static void search_end(void)
{
	k_mutex_lock(&loc_lock, K_FOREVER);
	search_active = false;
	agnss_pending = false;
	k_mutex_unlock(&loc_lock);
}

static void store_fix(const struct location_data *loc)
{
	struct app_fix f = {
		.lat = loc->latitude,
		.lon = loc->longitude,
		.accuracy_m = loc->accuracy,
	};
	int64_t now_ms;

#if defined(CONFIG_LOCATION_DATA_DETAILS)
	f.ttff_ms = (uint32_t)loc->details.gnss.pvt_data.execution_time;
	if (loc->details.gnss.pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		f.alt_m = loc->details.gnss.pvt_data.altitude;
		f.has_alt = true;
	}
#endif
	if (date_time_now(&now_ms) == 0) {
		f.timestamp = now_ms / 1000;
	}

	k_mutex_lock(&loc_lock, K_FOREVER);
	pending_fix = f;
	fix_ready = true;
	have_last_coords = true;
	last_lat = f.lat;
	last_lon = f.lon;
	/* A fresh fix is precise; a small non-zero uncertainty lets the server
	 * prune hard while leaving the elevation mask a little margin. */
	last_unc_m = (f.accuracy_m > 1.0) ? (uint32_t)f.accuracy_m : 50;
	k_mutex_unlock(&loc_lock);
}

static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_STARTED:
		LOG_DBG("Location search started");
		break;

	case LOCATION_EVT_LOCATION:
		LOG_INF("Got fix: %.06f, %.06f (acc %.1f m)",
			(double)event_data->location.latitude,
			(double)event_data->location.longitude,
			(double)event_data->location.accuracy);
		store_fix(&event_data->location);
		search_end();
		break;

#if defined(CONFIG_NRF_CLOUD_AGNSS)
	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		k_mutex_lock(&loc_lock, K_FOREVER);
		if (agnss_fetched) {
			/* Already served a (partial) set this search; the modem will
			 * fix with what it has. Ignore to avoid a fetch loop. */
			LOG_DBG("A-GNSS re-request ignored (already fetched this search)");
		} else {
			agnss_frame = event_data->agnss_request;
			agnss_pending = true;
			LOG_DBG("A-GNSS assistance request pending");
		}
		k_mutex_unlock(&loc_lock);
		break;
#endif

	case LOCATION_EVT_TIMEOUT:
		LOG_WRN("Location search timed out");
		search_end();
		break;

	case LOCATION_EVT_ERROR:
		LOG_WRN("Location search failed");
		search_end();
		break;

	case LOCATION_EVT_RESULT_UNKNOWN:
		LOG_DBG("Location result unknown");
		search_end();
		break;

	default:
		LOG_DBG("Location event %d", event_data->id);
		break;
	}
}

int app_location_init(void)
{
	int err = location_init(location_event_handler);

	if (err) {
		LOG_ERR("location_init failed: %d", err);
		return err;
	}
	LOG_INF("Location library initialised");
	return 0;
}

int app_location_request(bool single_shot)
{
	struct location_config config;
	enum location_method methods[] = { LOCATION_METHOD_GNSS };
	int err;

	location_config_defaults_set(&config, 1, methods);
	/* Bound the GNSS search so it cannot hang waiting for a "complete"
	 * assistance set that the pruned response will never provide. */
	config.methods[0].gnss.timeout = 120 * MSEC_PER_SEC;
	/* On the first fix after boot the modem is still active on LTE (attach,
	 * telemetry, the A-GNSS fetch itself), so GNSS gets only short RF windows
	 * between LTE idle-mode operations and can take ~60 s even with valid
	 * assistance. Priority mode lets GNSS pre-empt LTE if five consecutive PVTs
	 * are blocked; the library auto-disables it after the first fix or 40 s, so
	 * it only affects the slow first acquisition. */
	config.methods[0].gnss.priority_mode = true;

	search_reset(single_shot);

	err = location_request(&config);
	if (err) {
		LOG_ERR("location_request failed: %d", err);
		search_end();
		return err;
	}
	LOG_INF("GNSS search requested (%s)", single_shot ? "single-shot" : "2h window");
	return 0;
}

bool app_location_agnss_pending(struct nrf_modem_gnss_agnss_data_frame *frame,
				bool *single_shot)
{
	bool pending;

	k_mutex_lock(&loc_lock, K_FOREVER);
	pending = agnss_pending;
	if (pending) {
		if (frame) {
			*frame = agnss_frame;
		}
		if (single_shot) {
			*single_shot = current_single_shot;
		}
		agnss_pending = false;
	}
	k_mutex_unlock(&loc_lock);
	return pending;
}

void app_location_agnss_mark_fetched(void)
{
	k_mutex_lock(&loc_lock, K_FOREVER);
	agnss_fetched = true;
	k_mutex_unlock(&loc_lock);
}

bool app_location_get_fix(struct app_fix *fix)
{
	bool ready;

	k_mutex_lock(&loc_lock, K_FOREVER);
	ready = fix_ready;
	if (ready) {
		if (fix) {
			*fix = pending_fix;
		}
		fix_ready = false;
	}
	k_mutex_unlock(&loc_lock);
	return ready;
}

bool app_location_last_coords(double *lat, double *lon, uint32_t *unc_m)
{
	bool have;

	k_mutex_lock(&loc_lock, K_FOREVER);
	have = have_last_coords;
	if (have) {
		if (lat) {
			*lat = last_lat;
		}
		if (lon) {
			*lon = last_lon;
		}
		if (unc_m) {
			*unc_m = last_unc_m;
		}
	}
	k_mutex_unlock(&loc_lock);
	return have;
}
