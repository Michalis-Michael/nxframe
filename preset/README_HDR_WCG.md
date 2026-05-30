# NxFrame HDR / WCG Presets

These presets exercise the HDR/WCG signalling path added for HEVC/x265 live contribution.
They are intended as experimental broadcast-development presets, not final production certification presets.

## Presets

- `x265_1080p50_hlg_main10_bt2020_lowlatency.json`
  - HEVC Main10
  - 10-bit 4:2:0
  - BT.2020 primaries
  - HLG transfer
  - BT.2020 non-constant-luminance matrix
  - Low-latency IP GOP

- `x265_1080p50_pq_main10_bt2020_lowlatency.json`
  - HEVC Main10
  - 10-bit 4:2:0
  - BT.2020 primaries
  - PQ / ST 2084 transfer
  - BT.2020 non-constant-luminance matrix
  - Low-latency IP GOP

- `x265_1080p50_hlg_main10_bt709_lowlatency.json`
  - HEVC Main10
  - 10-bit 4:2:0
  - BT.709 primaries
  - HLG transfer
  - BT.709 matrix
  - Low-latency IP GOP

## Example

From the project root or build folder, use the preset name without `.json`:

```bash
./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x265_1080p50_hlg_main10_bt2020_lowlatency
```

Expected log indicators:

```text
Profile: main10
Target: yuv420p10le
Primaries: bt2020
Transfer: hlg
Matrix: bt2020nc
```

## Notes

HLG and PQ/ST2084 require 10-bit output. The encoder now rejects HDR transfer functions with 8-bit output.
PQ/ST2084 support here means transfer-function signalling. Full HDR10 mastering-display metadata, MaxCLL, and MaxFALL are not yet implemented.

## HDR10 / PQ metadata

The PQ/ST2084 preset now includes an optional `hdr10` block. NxFrame passes this to libx265 using `master-display` and `max-cll`, and the receiver/decoder path can preserve HDR10 mastering-display and content-light side data when it is present on decoded frames.

The default values are generic BT.2020-container HDR10 test values. For production, replace them with values that match the mastering monitor and programme content.
