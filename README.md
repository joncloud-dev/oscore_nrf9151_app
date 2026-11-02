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
loc single             # GNSS fix, single-shot A-GNSS (smallest payload)
loc window             # GNSS fix, 2h-window A-GNSS
loc setpos <lat> <lon> [unc_m]   # seed the last-known position hint (no arg: show)
```

`loc setpos` manually seeds the last-known position used as the A-GNSS/P-GPS
reference-location assistance and the server pruning anchor. Useful on a device
that has no fix yet this power cycle (e.g. to test whether injecting a coarse
position ahead of the P-GPS ephemerides speeds up the first fix). Called with no
arguments it prints the currently stored coordinates.

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

## Predicted GNSS (P-GPS) and the assistance soak test

Predicted assistance (`POST /p`, [agnss-schema.md §3](../agnss-schema.md)) is
the same compact ephemeris encoding as A-GNSS, but delivered as one broadcast
parameter set per satellite per **4 h window**, days ahead of time. The device
downloads a horizon of windows once, over a cheap bearer, then fixes from the
stored window covering the current time without contacting the cloud at all.

**Flash ring.** Windows go into the `pgps_storage` partition on the DK's
external SPI flash (see
[`boards/nrf9151dk_nrf9151_ns.overlay`](boards/nrf9151dk_nrf9151_ns.overlay)),
one 4 KiB flash sector per window. The partition is used as a ring indexed by
window number, so slots expire on their own and no bookkeeping is needed:
`CONFIG_OSCORE_CLOUD_PGPS_STORE_SLOTS=84` is exactly 14 days, the maximum the
`/p` contract defines. The horizon survives reboots, which is the point — a
device can be held offline for days and still fix.

**Batched download.** A whole horizon cannot arrive in one response: 84 windows
is ~97 kB, far past the 12 KiB Block2 accumulation buffer. `pgps fetch <n>`
therefore issues a sequence of requests of `CONFIG_OSCORE_CLOUD_PGPS_FETCH_BATCH`
windows each (default 6 = 24 h) with an advancing `from`, one request per
main-loop iteration, so telemetry and any in-flight GNSS search keep being
serviced in between.

```
pgps fetch [n] [all]   # download n windows (batched); `all` skips server pruning
pgps fix               # GNSS fix fed by the stored window covering now
pgps store             # show the stored horizon (slots used, toe range, generation)
pgps store erase       # wipe the ring
```

Note that the server publishes a 5-day horizon by default (`PGPS_HORIZON`), so
`pgps fetch 84` stores ~30 windows today; the full 14 days needs the Orekit
prediction source. The fetch stops on its own when the server runs out.

### Assistance soak test

`soak` runs the measurement
[`pgps-implementation-plan.md` §8.4](../pgps-implementation-plan.md) asks for:
fix and uplink a position every N minutes for hours or days, with the assistance
source pinned for the whole run. Run two DKs side by side — one on `pgps`, one
on `agnss` — and both see the same sky, so the TTFF comparison is fair.

```
soak start <pgps|agnss|agnss-window|none> <interval_min> [duration_h] [all]
soak stop
soak status
```

```
soak start pgps 60 72     # DK A: hourly fixes from stored windows, for 3 days
soak start agnss 60 72    # DK B: hourly fixes with a live single-shot A-GNSS fetch
soak start none 60        # control: no assistance at all
```

The pinned mode is never silently swapped. If the chosen assistance is
unavailable — no stored window covers now, or no link for an A-GNSS fetch — the
cycle fixes unassisted and *reports that it did*, so a fallback shows up in the
data instead of quietly contaminating it. `soak status` prints the run counters
and the stored horizon; state is RAM-only, so a reboot ends the run (the flash
windows survive, so restarting picks up where it left off).

Each cycle's uplink carries free-form telemetry metrics alongside the fix:

| Key | Value |
|-----|-------|
| 30 | assistance actually used: 0 none, 1 A-GNSS, 2 P-GPS |
| 31 | cycle index within the run |
| 32 | age of the injected prediction, minutes |
| 33 | ephemerides injected |
| 34 | 1 = the pinned assistance was applied, 0 = fell back unassisted |

Key 32 is the axis the experiment turns on: it is how stale the prediction was
when it fed the fix, so the backend can plot TTFF against days-since-generation
and join on `gen`/`toe` against the server's `pgps_windows` collection.

## LTE link handling

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

Two non-secure board targets (TF-M + app):

```
west build -b nrf9151dk/nrf9151/ns      # nRF9151 DK
west build -b thingy91x/nrf9151/ns      # Thingy:91 X
west flash
```

Both use devicetree partitions; Partition Manager is deprecated and off
(`SB_CONFIG_PARTITION_MANAGER` has no `default y` in NCS v3.4.0), so the
Thingy:91 X board's `sysbuild.cmake` static YAML is never applied.

The nRF9151 is the same 1 MiB part on both boards, and both targets use **one
flash layout**, defined in
[`boards/oscore_flash_layout.h`](boards/oscore_flash_layout.h) and built from
those numbers by a small per-board `.dtsi`. One image-size budget, and two soak
units that behave identically:

```
internal (1 MiB)                    external SPI NOR (32 MiB)
  0x000000  boot_partition   64 KB    0x000000  slot1_partition   896 KB
  0x010000  slot0_partition 896 KB      0x000000  slot1_s         192 KB
    0x010000  slot0_s       192 KB      0x030000  slot1_ns        704 KB
    0x040000  slot0_ns      704 KB    0x0e0000  pgps_storage      336 KB
  0x0f0000  tfm_ps/its/otp   32 KB    0x134000  free            ~30.8 MB
  0x0f8000  storage          32 KB
```

**The MCUboot secondary slot is on external flash.** Keeping it internal costs
448 KB of the 1 MiB and leaves the application only 256 KB — enough for the DK
with ~27 KB spare, but the Thingy:91 X additionally links the nPM1300 charger,
I2C, MFD and regulator drivers and overflows by ~1.6 KB. Moving it out gives the
application **704 KB** on both boards and keeps a full-size upgrade slot for when
FOTA is re-enabled. MCUboot therefore needs the SPI NOR driver, and it must see
the same slot bounds as the application or it writes its image trailer outside
the slot — board `.overlay` files reach only the application image, so
[`sysbuild.cmake`](sysbuild.cmake) hands the layout and the driver config to the
`mcuboot` image too.

`slot0_s` / `slot0_ns` and the TF-M and settings partitions keep the addresses
the DK ran before, so the application's link address is unchanged and provisioned
OSCORE key material in TF-M storage survives the layout change.

This app also needs TF-M's **full** profile — uoscore-uedhoc keeps the OSCORE
master secret as an opaque PSA key, so it needs the TF-M crypto service, and the
provisioned key is a persistent PSA key in Internal Trusted Storage — which
neither stock layout accommodates:

| | nRF9151 DK | Thingy:91 X |
|---|---|---|
| Stock bootloader chain | MCUboot at 0x0 | b0 + updatable MCUboot (s0/s1) |
| Stock secure flash / SRAM | 256 KB / 88 KB | 32 KB / 32 KB (minimal profile) |
| Stock TF-M PS/ITS/OTP | present, at 0xf0000 | **absent** |
| Stock settings partition | internal, 32 KB | external, 8 KB |
| Stock secondary slot | internal | external |
| App RAM | 128 KB | ~150 KB |

The Thingy:91 X needs its whole internal layout rebuilt, for two reasons. NCS
derives TF-M's PS/ITS/OTP areas from the devicetree nodelabels
`tfm_ps_partition` / `tfm_its_partition` / `tfm_otp_partition`
(`nrf/modules/trusted-firmware-m/tfm_boards/partition/flash_layout.h`), and the
factory layout defines **none** of them — `b0 + s0 + s1 + slot0` consume all
1 MiB — so a full-profile build fails with `TFM_OTP_NV_COUNTERS_AREA_ADDR must be
defined in flash_layout.h`. And MCUboot built as a *second stage* (under b0,
running from s0/s1) requires `zephyr,code-partition` to be `s0_partition` or
`s1_partition`, while its own `app.overlay` chooses `&boot_partition`, which the
factory layout lacks. `SB_CONFIG_SECURE_BOOT_APPCORE=n` in
[`sysbuild.conf`](sysbuild.conf) drops b0 to match (a no-op on the DK).

One MCUboot gotcha worth recording, since it cost a silent non-booting image:
MCUboot's own `boot/zephyr/app.overlay` is the only thing that chooses
`zephyr,code-partition = &boot_partition`, and Zephyr applies it only when there
is no board-specific overlay in `boot/zephyr/boards/`. MCUboot ships one for the
Thingy:91 X (it raises the UART baud), so `app.overlay` is skipped there and
code-partition falls back to `&slot0_partition` from the board devicetree —
linking MCUboot on top of the application image and leaving nothing at the reset
vector. The bootloader overlays in `boards/*_bl_partitions.overlay` set it
explicitly for both boards, so this does not depend on which overlays upstream
happens to ship.

Note also that MCUboot runs UART0 at 1 Mbaud on the Thingy:91 X (that upstream
overlay again) while the application runs at the board's 115200. MCUboot has no
console — it uses the port for mcumgr serial recovery — so nothing is lost, but
recovery and the application console are at different rates.

Consequence worth knowing: the Thingy:91 X image is **not** interchangeable with
factory firmware over USB DFU, since both the bootloader chain and the slot
bounds change. Flash the whole image (`west flash`, or a debugger).

With 704 KB of application flash, the coredump subsystem that `prj.conf` disables
to fit the old 256 KB slot can now be re-enabled on either board.

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
| `CONFIG_OSCORE_CLOUD_PGPS` | OSCORE predicted-GNSS client (POST `/p`) — on in this sample |
| `CONFIG_OSCORE_CLOUD_PGPS_STORE` | Keep predicted windows in the `pgps_storage` flash ring (on; needs the board overlay) |
| `CONFIG_OSCORE_CLOUD_PGPS_STORE_SLOTS` | Ring length = horizon: 84 slots x 4 KiB = 14 days (336 KiB) |
| `CONFIG_OSCORE_CLOUD_PGPS_FETCH_BATCH` | Windows per `/p` request when splitting a long fetch (default 6) |
| `CONFIG_LOCATION` / `CONFIG_LOCATION_METHOD_GNSS` | GNSS location search |

FOTA (`CONFIG_OSCORE_CLOUD_FOTA`) is available in the library but disabled in
this minimal sample.
