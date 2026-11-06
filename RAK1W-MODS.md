# RAK 1W (RAK3401 + RAK13302) reliability mods

This repo is [MeshCore](https://github.com/meshcore-dev/MeshCore) with extra
changes for RAK 1 watt solar nodes. Everything else is stock MeshCore.

## Mod 1: Low-voltage cutoff with resume hysteresis

**Problem:** Stock firmware has no low-voltage protection on the RAK3401
(`NRF52_POWER_MANAGEMENT` isn't enabled for it, and MeshCore has no runtime
cutoff on any nRF52 board). When the battery sags to ~3.4-3.6 V, 1 W transmit
pulses brown the node out, it reboots on the rebounded voltage, transmits,
and browns out again - the node flickers and the battery never recovers.

**Fix:**

| State | Rule |
|---|---|
| Running | Battery checked every 60 s. Below **3.4 V** for 3 checks in a row -> hibernate |
| Hibernating | Radio, PA, 5 V boost and 3V3_S rail off, MCU asleep. Re-checks every 5 min. Reboots at **3.8 V** or when USB is plugged in |
| Cold boot | Won't start below **3.6 V** (USB power skips this) |

After a recovery, `get pwrmgt.bootreason` shows `Shutdown: Low Voltage`.

Thresholds live in `variants/rak3401/variant.h`, or override them in
`variants/rak3401/platformio.ini` build flags:

```
-D PWRMGT_VOLTAGE_CUTOFF=3400
-D PWRMGT_VOLTAGE_RESUME=3800
-D PWRMGT_VOLTAGE_BOOTLOCK=3600
-D PWRMGT_CHECK_INTERVAL_SECS=60
-D PWRMGT_CUTOFF_CONSECUTIVE=3
-D PWRMGT_HIBERNATE_CHECK_SECS=300
```

Why not SYSTEMOFF + LPCOMP wake (MeshCore's built-in approach)? LPCOMP only
offers coarse VDD-ratio steps (~3.57 / 3.92 / 4.28 V here) that depend on
divider tolerance. Too high and the node never wakes. Polling the ADC gives
exact thresholds that match the telemetry voltage and can't get stuck off.

### Bench test before deploying

Bench supply on the battery connector, **USB unplugged**, build with
`-D MESH_DEBUG=1 -D PWRMGT_HIBERNATE_CHECK_SECS=30`:

1. ~3.3 V -> shuts down within ~3 min
2. 3.7 V -> stays off
3. ~3.85 V -> reboots (allow up to the 30 s check interval)

Thresholds compare against the node's stock battery reading (no calibration
needed). On the bench that reading tracked the supply closely around
3.4-3.8 V and read ~0.15 V low near 4.35 V, so the cutoff/resume points were
picked in the range where the stock reading is accurate.

## Building

```bash
pip install platformio
pio run -e RAK_3401_repeater
```

Output is in `.pio/build/RAK_3401_repeater/` (`firmware.zip` for BLE OTA,
`firmware.hex` for USB). For a UF2: `pio run -e RAK_3401_repeater -t create_uf2`.
Other targets: `RAK_3401_room_server`, `RAK_3401_companion_radio_ble`,
`RAK_3401_sensor`.

## Pulling in new MeshCore releases

```bash
git remote add upstream https://github.com/meshcore-dev/MeshCore.git   # once
git fetch upstream
git merge upstream/main
```

The mods only touch `variants/rak3401/` and this file, so merges are usually
clean.
