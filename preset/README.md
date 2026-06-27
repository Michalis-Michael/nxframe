# NxFrame presets

The `preset/` folder contains JSON encoder presets used by sender mode. Presets
are intentionally grouped by operational purpose instead of codec name alone.

## Folder layout

- `01_low_latency_contribution/` - primary low-latency broadcast contribution
  presets.
- `02_quality_contribution/` - higher-quality presets where a little more CPU or
  delay may be acceptable.
- `03_cpu_safe_development/` - conservative presets for older CPUs and testing.
- `04_hdr_wcg/` - experimental HDR/WCG x265 presets.
- `05_quality_contribution_high_latency/` - high-latency quality/compatibility
  presets.

## Notes

The main NxFrame sender path is optimized around the internal `YUV422P10LE`
10-bit 4:2:2 bus. Presets that request another pixel format may require an
explicit conversion path inside the selected encoder. Use those presets only
when compatibility is more important than the lowest-latency path.

`receiver_audio_route_example.json` is an example receiver-side audio routing
file. It is not an encoder preset.
