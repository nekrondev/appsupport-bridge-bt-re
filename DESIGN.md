# appsupport-bridge-bt — clean-room C reimplementation (Design A: faithful shared-radio clone)

Reverse-engineered from Jolla's `/usr/sbin/appsupport-bridge-bt`
(Sailfish OS 5.2.0.22, aarch64, stripped C++; sha256 `f96028e5…`). This document is the
build spec. Goal: a maintainable C daemon that reproduces the bridge's behaviour and lets us
fix Bluetooth issues (starting with the BLE-advertising bug) by editing source instead of
byte-patching.

## 1. Role

One Bluetooth controller (`hci0`) is shared between **three** consumers:

```
   Android container (App Support)                 host Sailfish BlueZ
   com.android.bluetooth (GD stack)                bluetoothd
        │ HIDL IBluetoothHci@1.0 (gbinder)              │ (manages hci1)
        ▼                                               ▼
   ┌─────────────────────────── appsupport-bridge-bt ───────────────────────────┐
   │  gbinder service  "…IBluetoothHci/default" on /dev/hwpuddlejumper           │
   │  vhci (/dev/vhci)  ──creates──►  hci1   (BlueZ attaches here)               │
   │  HCI_CHANNEL_USER  ──exclusive──►  hci0  (the real MediaTek controller)     │
   │  BlueZ D-Bus client (org.bluez): rfkill + LEAdvertisingManager1            │
   │  glib main loop; command-credit + queue arbitration                        │
   └────────────────────────────────────────────────────────────────────────────┘
```

- **Controller side (hci0):** `socket(AF_BLUETOOTH, SOCK_RAW|NONBLOCK|CLOEXEC, BTPROTO_HCI)`,
  `ioctl(HCIDEVDOWN, 0)`, then `bind()` a `sockaddr_hci{family=31, dev=0, channel=HCI_CHANNEL_USER=1}`.
  glib io-watch (`G_IO_IN|HUP|ERR`, prio 0xCC) → controller-event handler.
- **BlueZ side (hci1):** created as a **vhci** via `/dev/vhci`. BlueZ drives hci1; the bridge
  relays hci1↔hci0 and talks D-Bus to `org.bluez` (`/org/bluez/hci1`).
- **Container side:** gbinder local object implementing `android.hardware.bluetooth@1.0::IBluetoothHci`,
  registered as `default` on `/dev/hwpuddlejumper`. Transaction codes observed:
  `1=initialize(callbacks)`, `2=sendHciCommand(vec<u8>)`, `3=sendAclData`, `4=sendScoData`, `5=close`.
  Callbacks (IBluetoothHciCallbacks): `initializationComplete(Status)`, `hciEventReceived(vec<u8>)`,
  `aclDataReceived`, `scoDataReceived`. Send-to-container helper: internal `fn@0x4145e4(ctx,type,buf,len)`.

## 2. HCI command dispatch (container → bridge)

Incoming container command (`fn@0x41f600`): validate length, read opcode, switch on **OGF**
(`opcode>>10`): OGF 1/3/4/5/6/8/0x3f handled; OGF 8 (LE) → sub-dispatch by **OCF** (`opcode&0x3ff`)
through a 123-entry jump table (`.rodata@0x446804`, `target = 0x422b6c + i16*4`). Each LE OCF has a
dedicated case that either **forwards** to the controller (`→fn@0x41eb24`, availability-checked) or
**synthesizes** a Command-Complete (`→fn@0x41b660`, which `g_idle_add`s a per-opcode generator that
calls the send-to-container helper).

## 3. THE ADVERTISING BUG (what this reimplementation must get right)

- **Controller truly supports LE extended advertising**: 16 sets, MaxAdvLen 251, 1M/2M/Coded,
  HardwareOffload (confirmed via BlueZ `LEAdvertisingManager1` on hci1).
- **Bug 1 — `LE Read Number of Supported Advertising Sets` (0x203b) is stubbed to 0.**
  Generator `fn@0x41a950` writes CC `0e 05 01 3b 20 00 00` — `sturh wzr,[sp,#5]` forces
  status=0 **and num_sets=0**. The GD stack then can't allocate any advertiser.
- **Bug 2 — extended advertising is unimplemented.** `0x2036` (Set Ext Adv Params) et al. route to a
  **failure** generator (`fn@0x424c84 → 0x41b1f0`, logs "Untested command / Returning failure"),
  producing a malformed CC → the GD stack aborts (`on_set_extended_advertising_parameters_complete:
  IsValid()`). Only the **legacy** advertising path (`0x2006/08/0a`) is fully implemented and routes
  to real generators that store params/data and register the advert with **BlueZ**
  (`LEAdvertisingManager1.RegisterAdvertisement`, `org.bluez.LEAdvertisement1`).

**Correct behaviour for the reimplementation (two valid strategies — pick one, keep the other
documented):**
- **(preferred) Implement extended advertising properly:** report real caps for 0x203a/0x203b,
  accept 0x2036/37/38/39 and register the advertisement with BlueZ; return well-formed CCs
  (0x2036 CC = `status, selected_tx_power`). Modern, matches controller capability.
- **(fallback, proven on-air) Present legacy-only:** clear the LE Extended-Advertising feature bit in
  the `0x2003` reply and zero vendor `max_advt_instances`, so the stock GD stack uses the legacy path
  we already know radiates through BlueZ. Simpler; loses extended-only features.

## 4. Other virtualization the clone must preserve (from strings/behaviour)

- Scan: converts extended↔legacy advertising reports for the container; "Routing … to BlueZ only
  (AppSupport LE scan inactive)"; honours whether container LE scan is active.
- Resolving list / whitelist: virtualized; "synthetic success" for already-applied entries;
  injected remove commands before completing clear (0x2029).
- Per-model quirks from `/etc/appsupport-bridge-bt.conf` (`BrokenExtendedLECreateConnectionModels`,
  keyed on `getprop ro.product.model`); `0x2043` extended-create-connection → legacy fallback.
- rfkill coordination so hci1/hci0 power state stays consistent; command-credit + tx queue tracking.

## 5. Dependencies / build

aarch64, links: `libgbinder` (container HIDL), `glib-2.0`/`gobject`/`gio` (loop + GDBus to BlueZ),
`libglibutil`. Drop Qt5 (original used Qt D-Bus; we use **GDBus**). Cross-built on `monica` with
`aarch64-linux-gnu-gcc`, headers from upstream `libgbinder`/`libglibutil` + distro glib, linked
against the device's aarch64 `.so`s. Install path `/usr/sbin/appsupport-bridge-bt`; service
`appsupport-bridge-bt.service`.

## 6. Build stages

1. **Scaffold + HCI channel** (this stage): glib loop, exclusive hci0 user-channel, event read/log.
2. **gbinder IBluetoothHci service**: register, handle initialize/sendHciCommand/close, deliver events.
3. **vhci + BlueZ relay**: create hci1, relay hci1↔hci0, GDBus to org.bluez.
4. **HCI mux + opcode handling**: forward/synthesize per OCF; **advertising fixed** per §3.
5. **rfkill + quirks + config**; soak-test advertise→connect→re-advertise vs. the stock bridge.

## 7. Implementation status

| Feature | File | Status |
|---|---|---|
| hci0 `HCI_CHANNEL_USER` takeover | hci.c | ✅ verified on device |
| vhci (hci1) for host BlueZ | vhci.c | ✅ verified |
| gbinder `IBluetoothHci@1.0` service | gbinder_svc.c | ✅ verified (container BT comes up ON) |
| HCI command mux + credits + response demux | mux.c | ✅ verified |
| Connection/scan/adv ownership routing | mux.c | ✅ verified (16 adv sets exposed) |
| Advertising | (native forward) | ✅ 16 sets, extended — fixes the bug by construction |
| Scan enable/disable arbitration | mux.c | ✅ unit-tested (`test/mux_test.c`) |
| Resolving-list / accept-list virtualization | mux.c | ✅ unit-tested |
| Scan-report ext→legacy conversion | mux.c | ✅ unit-tested |
| Vendor-cap APCF filter hiding (0xFD53) | mux.c | ✅ unit-tested |
| Command-complete demux + credit flow | mux.c | ✅ unit-tested + verified on device |
| Per-model quirk (ext-conn→legacy) + config | config.c + mux.c | ✅ conversion unit-tested (quirk off for this model); config-file read not unit-tested |
| rfkill coordination | rfkill.c | ⚠️ compiled; I/O-bound, not unit-tested (needs a device rfkill toggle to exercise) |

Validation: `make test` builds `test/mux_test.c` (host-native, stubs the I/O) and runs 26 assertions
over the mux logic — scan arbitration, list virtualization, report conversion, APCF hiding, the
ext-conn quirk, connection ownership routing, and command demux. It also runs in CI.

Interface parity: the stock Jolla bridge publishes ONLY `android.hardware.bluetooth@1.0::IBluetoothHci`
(+ `IBluetoothHciCallbacks`) — verified by string-inspecting the stock binary; it has no `@1.1` and no
AIDL descriptor. This reimplementation matches that exactly (@1.0 only). The container's GD stack
probes `@1.1` first, that lookup fails on both bridges, and it falls back to `@1.0`. (The @1.1/AIDL
service variants exist in MediaTek's *stock-Android* HALs, e.g. the Tecno dump — NOT in Jolla's bridge.)

Only genuine remaining difference vs Jolla: exact byte-for-byte parity of the internal state machine
(not behaviourally required — full migration verified working).

Test harness: `run-temp.sh` (stops Jolla's bridge, runs this one, restores on Ctrl-C).
Debug log tags: `CMD<-`, `EVT<-ctrl`, `SYNTH`, `CMD<-INT`, `rfkill:`, `config:`, `quirk:`,
`hid vendor APCF`.
