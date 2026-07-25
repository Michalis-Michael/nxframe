# NxFrame CLI example presets

The `preset/` folder contains ready-to-use JSON examples for the NxFrame CLI. Sender presets are intentionally grouped by operational purpose instead of codec name alone.

These files are separate from the protected browser-GUI templates in `gui/gui_encoder_presets/`. The GUI clones a protected template and saves the resulting per-channel working configuration under `config/channels/`.

## Folder layout

- `01_low_latency_contribution/` - primary low-latency broadcast contribution
  presets.
- `02_quality_contribution/` - higher-quality presets where a little more CPU or
  delay may be acceptable.
- `03_cpu_safe_development/` - conservative presets for older CPUs and testing.
- `04_hdr_wcg/` - experimental HDR/WCG x265 presets.
- `05_quality_contribution_high_latency/` - high-latency quality/compatibility
  presets.

## CLI use

From the repository root, select a sender preset by filename without `.json`:

```bash
cd /path/to/nxframe
./build/NxFrame send decklink 0 to 0.0.0.0:5000 \
  encoder preset x264_1080p50_pcm_cpu_safe
```

NxFrame searches the `preset/` tree recursively, so the category directory is not required. Keep filenames unique when using short names.

An explicit JSON path is also supported:

```bash
./build/NxFrame send decklink 0 to 0.0.0.0:5000 \
  encoder preset preset/03_cpu_safe_development/x264_1080p50_pcm_cpu_safe.json
```

`receiver_audio_route_example.json` is a receiver-side audio-routing example and can be selected with `--receiver-preset receiver_audio_route_example`.

## Notes

The main NxFrame sender path is optimized around the internal `YUV422P10LE`
10-bit 4:2:2 bus. Presets that request another pixel format may require an
explicit conversion path inside the selected encoder. Use those presets only
when compatibility is more important than the lowest-latency path.
