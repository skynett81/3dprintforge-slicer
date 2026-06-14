# Snapmaker U1 firmware integration notes

Snapmaker published the U1 firmware as three GPL forks (2026):

- Klipper:   https://github.com/Snapmaker/u1-klipper  (~20% modified)
- Moonraker: https://github.com/Snapmaker/u1-moonraker (~15% modified)
- Fluidd:    https://github.com/Snapmaker/u1-fluidd

**Licensing.** All three are GPL-3.0. The 3DPrintForge Slicer (OrcaSlicer fork)
and the 3DPrintForge Server are AGPL-3.0, which is GPL-compatible — we may read,
fork and port with attribution. Config/data (filament parameters, g-code macros,
error codes) is low-risk to mirror. We do **not** vendor the firmware source into
our products; we use it as the authoritative specification.

This file records what is worth using, what is already in use, and — most
importantly — the print-task contract our U1 g-code export must satisfy.

## What we already use (Server)

- **Error codes / diagnostics** — `server/moonraker-client.js` mirrors
  `u1-klipper/exception_manager.py`: severity levels (1=info, 2=pause, 3=cancel)
  and all 13 module IDs (522–533, 2052) -> wiki help-anchor slugs. Per-event
  human text comes from the printer at runtime (`coded_exception.py` carries a
  `message` field on every exception), so finer-grained messages need no static
  table.
- **Filament catalog** — `extractU1FilamentCatalog()` parses the live printer's
  `filament_parameters` (load/unload/clean/flow temps, `is_soft`, `flow_k` per
  nozzle, min/max). Exposed at `GET /api/printers/:id/u1/filament-catalog`.

## What NOT to do (Slicer)

**Do not push the firmware's `flow_k` into slicer `pressure_advance`.** On the
U1, `flow_k` *is* pressure advance, and the printer applies it itself from the
loaded filament. Snapmaker's own `@U1` filament presets therefore set
`enable_pressure_advance: "0"` on purpose. Injecting `flow_k` as slicer PA would
double-apply it. Likewise `flow_temp` is a flow-calibration temperature, not the
recommended print temperature — do not override print temps from it. (See the
"don't fake U1 parity via profile guesses" rule.)

## Print-task contract (what our U1 export must provide)

The U1's standby-preheat, auto-feed and auto-unload are **firmware-side** macros
(in `u1-klipper/lava/fluidd.cfg`), driven by a `print_task_config` object
(`klippy/extras/print_task_config.py`). The print job / slicer start g-code
populates it via these registered commands:

| Command | Sets |
|---|---|
| `SET_PRINT_EXTRUDER_MAP`     | `extruder_map_table` (logical -> physical tool) |
| `SET_PRINT_USED_EXTRUDERS`   | `extruders_used[]` (which tools the print uses) |
| `SET_PRINT_FILAMENT_CONFIG`  | per-tool `filament_vendor/type/sub_type/color(_rgba/_multi)/soft/sku/official` |
| `SET_PRINT_PREFERENCES`      | `auto_bed_leveling`, `flow_calibrate`, `shaper_calibrate`, `time_lapse_camera`, `end_unload_filament[]`, `auto_replenish_filament`, `filament_entangle_detect`, `end_led_turn_off` |
| `SET_PRINT_TASK_PARAMETERS`  | config-2: `line_width`, `layer_height`, `outer_wall_speed`, per-tool `nozzle_temp`, `nozzle_diameter`, `filament_diameter`, `filament_flow_ratio`, `filament_max_vol_speed`, `filament_used_g/mm` |
| `GET_PRINT_TASK_CONFIG` / `SAVE_CURRENT_PRINT_TASK_CONFIG` / `RESET_PRINT_TASK_CONFIG` / `LOAD_PRINT_TASK_CONFIG` | state management |

Why it matters:

- **Standby preheat** — `SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=n TEMP=t` only
  preheats a tool when `extruders_used[n]` is true. So correct
  `SET_PRINT_USED_EXTRUDERS` is what enables the U1's predictive preheat — there
  is nothing to implement slicer-side beyond emitting the truthful used-tool set.
- **Auto-feed / auto-unload** — `SM_PRINT_AUTO_FEED` and
  `SM_PRINT_END_AUTO_UNLOAD_FILAMENT` key off `extruders_used`, `filament_soft`
  and `end_unload_filament`. `filament_soft` should match the firmware's
  `is_soft` for the material (flexibles).
- **In-print flow / input-shaper calibration** — gated by `flow_calibrate` /
  `shaper_calibrate` in `SET_PRINT_PREFERENCES`.

### Verification result (checked against firmware + our profiles)

Our U1 machine profile (`resources/profiles/Snapmaker/machine/fdm_U1.json`)
starts with `PRINT_START TOOL_TEMP=... T0_TEMP=... TOOL=...` and then raw
`M104 T.. S..` + a manual purge. **The firmware's `PRINT_START` macro
(`lava/fluidd.cfg:193`) ignores those arguments entirely** — it only resets line
flags, runs `FLOW_APPLY_CALIBRATE_K` (the printer applying its own flow_k, again
confirming firmware-owned PA), then `M84/G92/M220`. It reads
`print_task_config['extruders_used']` but does not set it.

So: **our export never populates `print_task_config`.** Temperatures and purge
still work (we emit raw `M104`/moves), and **predictive next-tool preheat is
already handled engine-side** — OrcaSlicer 2.4 emits its own staged `M104` ahead
of each tool change, enabled for the U1 in `fdm_process_U1.json`
(`ooze_prevention=1, preheat_time=30`, commit e883e8fba8). So preheat is NOT the
gap.

What does NOT engage for prints we slice are the U1's *firmware-orchestrated*
extras that key off `print_task_config`: auto-feed (`SM_PRINT_AUTO_FEED`),
end-of-print auto-unload (`SM_PRINT_END_AUTO_UNLOAD_FILAMENT`), and in-print
flow/input-shaper calibration — because that data is normally set by Snapmaker's
own app/slicer via the native commands. (The firmware's own
`SM_PRINT_EXTRUDER_PREHEAT` would also be redundant with our engine preheat.)

To close it (non-faked: the printer still does the preheat, we only supply the
truthful task config), the U1 machine start g-code should call, **before** the
print moves (the firmware rejects these mid-print):

```
SET_PRINT_USED_EXTRUDERS EXTRUDERS=<comma-separated PHYSICAL extruder indices>
; e.g. EXTRUDERS=0,2 for a two-colour print using tools 0 and 2
SET_PRINT_FILAMENT_CONFIG ...   ; per-tool vendor/type/sub_type/colour/soft
SET_PRINT_PREFERENCES ...       ; flow_calibrate / end_unload_filament / etc (optional)
```

`SET_PRINT_USED_EXTRUDERS` parses `EXTRUDERS` as `int(v) for v in str.split(',')`
and sets `extruders_used[i]=True` for each; it raises a coded error if issued
while `print_stats.state in (printing, paused)`, and persists to `print_task.json`.

**Do NOT put this in the slicer's `machine_start_gcode`.** Verified against the
firmware: `print_stats.note_start()` sets `state="printing"` the moment the
print file begins, so anything in `machine_start_gcode` runs *during* printing
and `SET_PRINT_USED_EXTRUDERS` would be rejected (coded error) → aborted print.

**Wired server-side instead (correct place).** The 3DPrintForge Server sends it
via Moonraker BEFORE `/printer/print/start`:
`server/moonraker-client.js` → `deriveUsedExtruders(meta)` (used tool indices
from the file's per-tool filament weights, unit-tested) +
`_u1PreparePrintTaskConfig()` (U1-only, best-effort,
`SET_PRINT_USED_EXTRUDERS EXTRUDERS=<idx,..>` then start; never blocks the
print). Server commit `60501234`. **Still needs live U1 verification** that
auto-feed/auto-unload engage and nothing regresses.
