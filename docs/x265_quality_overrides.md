# HEVC quality and performance overrides

Based on `nxframe(20261003-083509).zip`, FFmpeg 8.1 and x265
4.2+18-6fdfffe8d. Only HEVC option parsing/forwarding, its tests and this document
are changed. Capture, telemetry, audio, muxing and transport are untouched.

## How overrides work

Use an `additional_options` object alongside `preset` in the video configuration.
FFmpeg establishes the selected x265 preset and tune first, then parses explicit
x265 parameters. Thus specified quality options override preset/tune defaults.
Omitted quality options are not appended by this wrapper. Existing explicit GOP,
rate-control, VUI and transport-header configuration still applies.

No thread setting is added or changed by this patch. Leave `threads`,
`frame-threads`, `pools`, `wpp`, `slices` and `lookahead-slices` out of the JSON for
preset/automatic behavior. Remove pre-existing thread overrides if you want auto;
this patch does not erase explicit settings from your existing preset.

Use ordinary JSON numbers or booleans. Hyphens and underscores are interchangeable
for the documented names, but supplying both spellings of an option is an error.
New boolean controls also accept their `no-`/`no_` forms: `"no-fast-intra": true`
means `fast-intra=0`; false means `fast-intra=1`. Conflicting positive/negative
forms are rejected. Unknown names, invalid ranges and parameter-string injection
are rejected rather than silently ignored. `x265_params` remains a supported JSON
object; `additional_options` replaces its entries when the exact key matches.

## New controls

| Option | Accepted values | Purpose / interaction |
|---|---|---|
| `rdoq-level` | 0..2 | Rate-distortion optimized quantization; higher levels add analysis work. |
| `rd-refine` | boolean | Additional QP refinement; requires RD > 4 and active adaptive quantization. |
| `early-skip` | boolean | Allows early termination of analysis for skip candidates. |
| `rskip` | 0..2 | Recursion skipping mode; 0 disables it. |
| `rskip-edge-threshold` | 0..100 | Edge-density threshold for rskip mode 2. |
| `fast-intra` | boolean | Faster intra-mode search. |
| `limit-refs` | 0..3 | Reference-search restrictions, interpreted as x265's bitmask. |
| `limit-modes` | boolean | Limits additional mode analysis where supported. |
| `limit-tu` | 0..4 | Transform-unit search restriction; requires sufficient inter TU depth. |
| `max-merge` | 1..5 | Maximum merge candidates. |
| `tu-intra-depth`, `tu-inter-depth` | 1..4 | Transform search depth; must also fit the selected CTU size. |
| `max-tu-size` | 4, 8, 16, 32 | Maximum transform size; cannot exceed CTU size. |
| `signhide` | boolean | Sign-bit hiding in quantized coefficients. |
| `weightp`, `weightb` | boolean | Weighted prediction; weightb has no purpose with B-frames disabled. |
| `cutree` | boolean | Temporal allocation of quantization; interacts with lookahead. |
| `aq-motion` | boolean | Motion-sensitive AQ behavior. |
| `tskip`, `tskip-fast` | boolean | Transform skipping and its fast-search control. |
| `sao-non-deblock` | boolean | Selects non-deblocked samples for SAO analysis. |
| `selective-sao` | 0..4 | x265's selective SAO policy; nonzero can enable SAO. |
| `nr-intra`, `nr-inter` | 0..2000 | Noise reduction; 0 disables it. Alters picture detail. |
| `cbqpoffs`, `crqpoffs` | -12..12 | Chroma QP offsets; negative allocates more chroma precision. |

## Existing controls remain supported

`rd` (1..6), `subme` (0..7), `me` (`dia`, `hex`, `umh`, `star`, `sea`, `full`),
`merange`, `ref`/`refs` (1..16), `rect`, `amp`, `aq-mode` (0..4),
`aq-strength` (0..3), `psy-rd` (0..5), `psy-rdoq` (0..50), `sao`, `no-sao`,
`limit-sao`, `deblock` (boolean), and strong-intra-smoothing toggles.

`rc-lookahead` remains supported but changes buffering/latency as well as analysis.
This patch does not reduce it automatically. `psy-rdoq` needs active RDOQ to have
its intended effect. x265 can adjust or disable incompatible combinations; inspect
its startup warnings and effective tool settings. Range validation does not imply
that every combination is useful, faster or accepted by x265's final checks.

The prior validator admitted `rd=0`, although the matching x265 library requires
1..6. This is corrected. Upper limits for AQ strength and psycho-visual strengths
now also match the library, so invalid requests fail before codec opening.

## Example: a small quality experiment, automatic threads

Keep the rest of your working superfast preset. Add only this object:

```json
"additional_options": {
  "rd": 3,
  "subme": 2,
  "aq-mode": 2,
  "aq-strength": 0.8,
  "sao": true
}
```

This is a test configuration, not a validated broadcast preset. It increases some
analysis work and enables quality tools; compare CPU, call-time tails, capture
drops and picture quality using the same SDI footage. To attribute changes, add
one setting at a time before testing the combination. Increasing analysis does
not guarantee a visible improvement on every source.

For faster-search experiments, `early-skip`, `fast-intra`, `rskip`, lower `subme`
and lower `rd` are available; some are already enabled by superfast. Setting a
value equal to its preset default cannot improve performance. Omit an option to
return to preset behavior, rather than guessing a replacement default.

## Build and validate

```sh
cmake --build build -j"$(nproc)"
ctest --test-dir build -R '^encoder_x265$' --output-on-failure
```

Regression coverage checks invalid ranges and aliases, explicit forwarding of
all new controls, absence of unrequested thread/lookahead overrides, unchanged
preset behavior when options are omitted, and successful decoding/order of an
encoded 10-bit 4:2:2 stream using the quality controls. Existing ownership,
captions, IDR, conversion, HDR and delayed-output tests remain in the suite.
