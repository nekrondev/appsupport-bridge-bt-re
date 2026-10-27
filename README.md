# appsupport-bridge-bt (reimplementation)

A small, from-source **C reimplementation of Jolla's Android App Support Bluetooth bridge**
(`appsupport-bridge-bt`) for Sailfish OS, meant to be run **temporarily** in place of the stock
bridge — primarily to provide the **BLE GATT advertising that Jolla's bridge does not yet
implement**.

## Why this exists

Sailfish OS runs Android apps in an "App Support" container that shares the phone's single
Bluetooth controller with the host system via Jolla's `appsupport-bridge-bt`. That bridge does
**not implement BLE peripheral / GATT advertising** for the container: it reports **0 advertising
sets** to the Android stack, so any Android app that needs to advertise as a BLE peripheral fails.

A concrete casualty is the Sparkasse **S-pushTAN** "Verbindung übernehmen" (device-to-device
transfer): the *new* device must advertise a GATT service so the *old* device can connect to it.
On the stock bridge that advertisement never goes on air, and the transfer cannot complete.

This reimplementation fixes that **the way Jolla's bridge intended** — no HCI shortcut. It
intercepts the container's extended-advertising commands and registers the advertisement with the
host's BlueZ (`org.bluez.LEAdvertisingManager1.RegisterAdvertisement`), the same mechanism Jolla
already uses for *legacy* advertising, while the GATT server stays in the Android container and the
incoming peripheral connection is routed back to it. It still multiplexes the one controller between
the container and host BlueZ. Verified end-to-end on the Jolla Phone 2026 (MediaTek MT6858, 16
extended advertising sets): advertise → a second phone connects → GATT services read → reconnect.

## What it does / doesn't

- **Temporary, runtime-only.** `run-temp.sh` stops the stock bridge, runs this one, and restores
  the stock bridge on Ctrl‑C. **Nothing is installed on disk**; a reboot returns you to stock.
- Reproduces the stock bridge's coexistence behaviour — HCI command mux with credit flow,
  per‑activity ownership routing (advertising / scan / connection), resolving‑list and
  filter‑accept‑list virtualization, extended→legacy scan‑report conversion, vendor APCF filter
  hiding, rfkill coordination, per‑model quirks — **plus** the advertising it was missing.
- Serves `android.hardware.bluetooth@1.0::IBluetoothHci` to the container (same interface the
  stock bridge exposes).
- Developed and verified on **Jolla Phone (2026), Sailfish OS 5.2.x, MediaTek MT6858**. Other
  devices/SoCs may need adjustment.
- **Not affiliated with, nor endorsed by, Jolla.**

See [`DESIGN.md`](DESIGN.md) for the full architecture and reverse‑engineering notes.

## Build

GitHub Actions (`.github/workflows/build.yml`) builds an aarch64 binary and uploads it as an
artifact. To build yourself you need an aarch64 toolchain plus `glib`, `libgbinder` and
`libglibutil`:

```sh
make MODE=pkgconfig            # native build; deps found via pkg-config
make                           # vendored cross-build (see Makefile; set BB=<build dir>, CROSS=)
```

## Use — at your own risk

Copy the built binary and `run-temp.sh` to the phone, then as root:

```sh
# the script expects the binary at /home/defaultuser/abbr (override with BIN=...)
devel-su sh run-temp.sh        # stops Jolla's bridge, runs this one; Ctrl-C restores Jolla's
```

Run your scenario (e.g. the pushTAN device transfer) while it's active, then press **Ctrl‑C** to
restore the stock bridge.

## ⚠️ Disclaimer

This software talks to your Bluetooth controller at the HCI level and temporarily replaces a
Sailfish OS system service. It is **experimental and unofficial**. **Use entirely at your own
risk.** It may disrupt Bluetooth; a reboot restores Jolla's stock bridge. Provided **as‑is, with
no warranty of any kind**, and not affiliated with Jolla or Sparkasse.
