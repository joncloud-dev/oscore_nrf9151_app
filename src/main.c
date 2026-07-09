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
 *   1. Bring up the modem and register on the LTE network.
 *   2. Best-effort wall-clock sync (for the optional telemetry timestamp).
 *   3. Configure the library and enter the connect / send / backoff loop.
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

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <date_time.h>

#include <oscore_cloud/cloud_config.h>
#include <oscore_cloud/cloud_session.h>
#include <oscore_cloud/cloud_client.h>
#ifdef CONFIG_OSCORE_CLOUD_DIAG
#include <oscore_cloud/diag_bridge.h>
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static K_SEM_DEFINE(lte_connected_sem, 0, 1);
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

static void lte_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type != LTE_LC_EVT_NW_REG_STATUS) {
		return;
	}

	if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
	    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
		LOG_INF("Network registered (%s)",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "home" : "roaming");
		k_sem_give(&lte_connected_sem);
	}
}

static void date_time_handler(const struct date_time_evt *evt)
{
	if (evt->type != DATE_TIME_NOT_OBTAINED) {
		k_sem_give(&time_ready_sem);
	}
}

static int network_up(void)
{
	int err;

	LOG_INF("Bringing up the modem...");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed: %d", err);
		return err;
	}

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed: %d", err);
		return err;
	}

	LOG_INF("Waiting for LTE network registration...");
	k_sem_take(&lte_connected_sem, K_FOREVER);

	/* Best-effort time sync; do not block the sample forever if it fails. */
	date_time_register_handler(date_time_handler);
	(void)date_time_update_async(NULL);
	if (k_sem_take(&time_ready_sem, K_SECONDS(30)) == 0) {
		LOG_INF("Wall-clock time obtained");
	} else {
		LOG_WRN("Time not obtained; telemetry will omit the timestamp");
	}

	return 0;
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
	uint32_t attempts = 0;
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

	err = network_up();
	if (err) {
		LOG_ERR("Network bring-up failed (%d); halting", err);
		return err;
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
		if (!connected) {
			err = oscore_cloud_connect();
			if (err) {
				uint32_t backoff = oscore_cloud_backoff_seconds(++attempts);

				LOG_WRN("Connect failed (%d); retrying in %u s (attempt %u)",
					err, backoff, attempts);
				k_sleep(K_SECONDS(backoff));
				continue;
			}

			LOG_INF("OSCORE cloud connected");
			attempts = 0;
			connected = true;
		}

		struct cloud_telemetry t;

		build_telemetry(&t);

		err = oscore_cloud_send_telemetry(&t, on_shadow_delta, NULL);
		if (err) {
			LOG_ERR("Telemetry send failed (%d); reconnecting", err);
			oscore_cloud_disconnect();
			connected = false;
			continue;
		}

		LOG_INF("Telemetry sent; next check-in in %u s", interval_s);
		k_sleep(K_SECONDS(interval_s));
	}

	return 0;
}
