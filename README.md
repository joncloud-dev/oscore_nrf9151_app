# oscore_nrf9151_app

A minimal reference application for the **nRF9151 DK** that talks to the OSCORE
platform (the Go/PocketBase server) using the reusable
[`oscore_cloud`](https://github.com/joncloud-dev/oscore_cloud) library.

It is the "batteries included" usage of the library: no zbus, no state-machine
framework — just a plain `main()` loop driving the `cloud_session` helper
(connect -> send telemetry -> apply any shadow delta), see
[`src/main.c`](src/main.c).

## What it does

1. Brings up the modem and attaches to the LTE network via the lightweight
   network module ([`src/network.c`](src/network.c)).
2. Best-effort wall-clock sync (for the optional telemetry timestamp).
3. Configures the OSCORE cloud client and enters a connect / send / backoff loop,
   posting a telemetry check-in every `CONFIG_APP_SAMPLE_INTERVAL_SECONDS`.

Telemetry, device shadow, CoAP, OSCORE and CBOR are all handled by the library;
this app only builds a `cloud_telemetry` record and reacts to shadow deltas.

## GNSS location + A-GNSS

The sample runs a GNSS-only location search
([`src/location.c`](src/location.c)) using the nRF Connect SDK Location library
and fetches A-GNSS assistance over OSCORE (POST `/a`) instead of nRF Cloud. A
completed fix is folded into the next telemetry uplink under map key 4
(agnss-schema.md 2.1).

Assistance is fetched in one of two modes, chosen per search:

- **Single-shot** (`loc single`) — the request asks the server for only the
  satellites visible *now*, capped to `CONFIG_APP_AGNSS_SINGLE_SHOT_MAX_SV`
  (default 9) and encoded compactly. For single-shot the server raises the
  elevation mask to 10° and keeps the strongest satellite per azimuth sector, so
  the set is the few highest-signal, quick-to-acquire SVs. This is the smallest
  payload (~0.5–0.7 kB) with a few-minutes validity, for devices that fetch
  fresh assistance on every fix.
- **2 h window** (`loc window`) — the request asks for every satellite that
  will rise within the next `CONFIG_OSCORE_CLOUD_AGNSS_WINDOW_S_DEFAULT` seconds
  (2 h), for a fix that stays usable across the window.

Trigger a search from the serial console, or set the device shadow desired
`{"loc":"single"}` / `{"loc":"window"}` to trigger one from the cloud:

```
loc single    # GNSS fix, single-shot A-GNSS (smallest payload)
loc window    # GNSS fix, 2h-window A-GNSS
```

**Position hint.** The A-GNSS request carries a coarse position so the server
can prune to visible satellites: the last GNSS fix if one exists this power
cycle, otherwise the serving cell captured by the network module. With no hint
the server falls back to the device's last-known fix (or sends the full
constellation). The server returns that resolved position back in the `loc`
element, and the app injects it as reference-location assistance so the modem
can constrain its search for a faster cold lock.

**Fast first fix.** The first search after boot competes with LTE (attach,
telemetry, the assistance fetch), so GNSS gets only short RF windows and can
take ~60 s even with valid assistance. The search enables the Location library's
GNSS **priority mode** (`config.methods[0].gnss.priority_mode`), which lets GNSS
pre-empt LTE if consecutive PVTs are blocked; it auto-disables after the first
fix or 40 s, so it only affects that slow first acquisition.

**Repeat-request suppression.** Because the server returns only a subset of the
constellation, the GNSS core keeps re-raising the assistance request for the
satellites it was not given. The location module fetches at most once per
search and ignores further requests, so the modem proceeds to a fix with the
partial set (assistance is optional). See [`src/location.c`](src/location.c).

### LTE link handling

LTE connectivity lives in a small, framework-free module
([`src/network.c`](src/network.c) / [`src/network.h`](src/network.h)) rather than
inline in `main()`. It follows the Asset-Tracker-Template's link semantics
without the zbus/SMF machinery:

- **PDN-based link state** — "connected" is driven by the modem's default-PDN
  activate/deactivate events (an IP bearer is actually usable), not merely by
  network-registration status. Exposed via `network_is_connected()` /
  `network_wait_connected()`.
- **Rich event handling** — a single `network_evt_handler_t` callback surfaces
  SIM failure, attach rejected, modem reset-loop, light/full search-done, and
  negotiated PSM/eDRX parameters.
- **Modem DFU-result handling** on init (a scheduled delta update applied during
  `nrf_modem_lib_init()` is classified instead of treated as a hard error).
- **Escalating recovery ladder** (`network_recover()`): wait for auto re-attach
  -> force an offline/normal cycle -> reinitialise the modem library.

The `main()` loop stays single-threaded: it polls/blocks on the link state and
asks the module to recover on persistent failure. LTE events arrive
asynchronously on the `lte_lc` workqueue.

## Device observability

The sample enables the library's optional observability layer
(`CONFIG_OSCORE_CLOUD_DIAG`): reboot reasons, a health-vitals heartbeat, minimal
crash reports and static device info are folded into the ordinary telemetry
uplink as nested CBOR maps (keys 20-23) and ingested by the PocketBase backend.
`main()` only calls `oscore_cloud_diag_init()` once at boot; the session helper
attaches everything to each telemetry post automatically. See the library's
README and the top-level `OBSERVABILITY.md` for the full picture.

Exercise it from the serial console with the `diag` shell group:

```
diag info                          # boot report, crash-pending, breadcrumbs, coredump size, live vitals
diag reboot [fota|user|software]   # planned reboot -> exercises reboot-reason attribution
diag coredump [erase]              # show (or erase) the stored coredump
diag fault <null|badjump|stack|oops|panic|assert|unaligned|secure>
```

Disable the whole stack with `CONFIG_OSCORE_CLOUD_DIAG=n` (and drop the
`diag`-related options in [`prj.conf`](prj.conf)), or take manual control of the
cadence by setting `CONFIG_OSCORE_CLOUD_DIAG_AUTO_ATTACH=n`.

## Dependencies / manifest

The [`west.yml`](west.yml) is a T2 (star) manifest. It:

- imports **sdk-nrf v3.4.0** (which imports Zephyr and all standard modules),
- **overrides** the `uoscore-uedhoc` pin with the
  [`joncloud-dev/uoscore-uedhoc`](https://github.com/joncloud-dev/uoscore-uedhoc)
  fork, branch `oscore-opaque-psa-keys` (opaque PSA master keys — required by the
  library's OSCORE wrapper), and
- adds the `oscore_cloud` library as a Zephyr module.

## Building

The board target is the non-secure nRF9151 DK image (TF-M + app):

```
west build -b nrf9151dk/nrf9151/ns
west flash
```

### Option A — dedicated workspace (portable / CI)

Because this repo is a west *manifest repo*, `west` treats its **parent
directory** as the workspace root and checks the whole NCS tree out there. Use
an empty directory so nothing else is polluted:

```
mkdir oscore-ws && cd oscore-ws
git clone https://github.com/joncloud-dev/oscore_nrf9151_app
west init -l oscore_nrf9151_app
west update
west build -b nrf9151dk/nrf9151/ns oscore_nrf9151_app
```

> Note: `west update` downloads the full NCS (~GBs) into `oscore-ws/`.

### Option B — build inside an existing NCS workspace (fast iteration)

If you already have an NCS v3.4.0 workspace (e.g. `~/work/ncs`) with the
`uoscore-uedhoc` fork checked out at `oscore-opaque-psa-keys`, you can build this
app without re-downloading NCS. Source [`env.sh`](env.sh), which points Zephyr at
the workspace and registers the standalone `oscore_cloud` library (a sibling
repo) as an extra module:

```
nrfutil toolchain-manager launch --shell
source env.sh
west build -b nrf9151dk/nrf9151/ns
```

Override paths if needed: `NCS_WORKSPACE=/path/to/ncs OSCORE_CLOUD_ROOT=/path/to/oscore_cloud source env.sh`.

> Do NOT source `att/env.sh` in the same shell: it registers the whole
> Asset-Tracker-Template repo as a module, which pulls in a second copy of
> `oscore_cloud` (`att/modules/oscore_cloud`) and causes duplicate-symbol errors.
> If a previous build mixed them in, do a pristine rebuild (`west build -p`).
>
> The workspace's `uoscore-uedhoc` must be the forked `oscore-opaque-psa-keys`
> branch, otherwise the OSCORE wrapper will not compile against the opaque-key API.

## Provisioning (one-time, on the bench)

OSCORE keys are **not** compiled in. Generate device material on the server
(`POST /api/provision`), then load it once over the shell (UART) and reboot:

```
oscore provision <secret_hex(16B)> <salt_hex> <sender_id_hex> <recipient_id_hex>
```

The master secret is imported as a non-exportable, persistent PSA key in the
TF-M secure domain; the salt and sender/recipient IDs go to PSA Protected
Storage. On the next boot the app loads them automatically. Until a device is
provisioned it logs an error and stays disconnected, but the shell stays up.

## Configuration

Key options (see [`prj.conf`](prj.conf) and [`Kconfig`](Kconfig)):

| Option | Purpose |
| --- | --- |
| `CONFIG_OSCORE_SERVER_IP` | CoAP/OSCORE server IPv4 address |
| `CONFIG_APP_SAMPLE_INTERVAL_SECONDS` | Telemetry check-in interval (default 600) |
| `CONFIG_APP_VERSION` | Fallback firmware version (used only when the build-id version is off) |
| `CONFIG_OSCORE_CLOUD_SESSION` | The framework-free connect/session helper (on) |
| `CONFIG_OSCORE_CLOUD_DIAG` | Device observability (reboot/vitals/crash/device-info) folded into telemetry (on) |
| `CONFIG_OSCORE_CLOUD_DIAG_VITALS_INTERVAL` | Telemetry sends between vitals heartbeats (1 = every send) |
| `CONFIG_OSCORE_CLOUD_AGNSS` | OSCORE A-GNSS client (POST `/a`) — on in this sample |
| `CONFIG_OSCORE_CLOUD_AGNSS_WINDOW_S_DEFAULT` | Default look-ahead window for the windowed mode (default 7200 = 2 h) |
| `CONFIG_OSCORE_CLOUD_AGNSS_COMPACT` | Request the compact positional-array response encoding (on) |
| `CONFIG_APP_AGNSS_SINGLE_SHOT_MAX_SV` | Satellite cap for `loc single` (default 9) |
| `CONFIG_LOCATION` / `CONFIG_LOCATION_METHOD_GNSS` | GNSS location search |

FOTA (`CONFIG_OSCORE_CLOUD_FOTA`) is available in the library but disabled in
this minimal sample.
