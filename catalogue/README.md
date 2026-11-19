# catalogue

`modes.json` (JSON, which is valid YAML 1.2): the modes the classifier
decodes or will classify, with their parameters. Every parameter says where
it comes from:

| provenance | meaning |
|---|---|
| `documented` | from the specification named in `source` (listed in `sources`) |
| `vendor` | from a decoder vendor's documentation (Wavecom), not the originator's |
| `observed` | measured on recordings by someone (analysts, sigidwiki, Priyom, blogs), as noted |
| `measured` | measured in this project on recordings supplied for it (`docs/RESEARCH.md` section 3.6) |
| `guess` | not established; to be checked |

`in_program` says what the program does with the mode today and how that
was tested. "Tested on synthetic signals only" means: each decoder's own
signal generator, through the full IQ chain (channelizer, detector, decoder
host) with white noise; no fading.

## What the program reads: `match`

Catalogue v1 adds, for every mode whose parameters the feature meter can
measure, one or more `match` blocks:

```json
"match": [
  {"modulation": "FSK", "shift_hz": [170], "baud": [100], "provenance": "vendor", "source": "WAVECOM"}
]
```

| key | |
|---|---|
| `modulation` | `FSK` (2 tones), `MFSK`, `PSK` (single carrier), `multi-carrier`, `OOK`: the families `core/src/features.cpp` reports |
| `shift_hz`, `baud`, `tones`, `tones_range`, `spacing_hz` | allowed values (lists; `tones_range` = [min, max]) |
| `frame_ms` | periods (ms) at which the mode repeats itself: its frame, when a known sequence is sent in every frame |
| `provenance`, `source`, `note` | where these numbers come from |
| `frame_provenance`, `frame_source` | where `frame_ms` comes from, when not from the same place as the rest |

The program embeds this file at build time (`core/src/catalogue.cpp`) and,
for every measured emission, lists the modes its features are consistent
with (tolerances in `core/include/hfc/catalogue.hpp`). That list is shown
for emissions no decoder recognised, as "consistent with", never as an
identification: SITOR-A, DSC-HF and CODAN Selcal, for one, share every
parameter it measures (100 Bd, 170 Hz FSK).

A frame period counts for a mode when the measured one agrees (within
2 %), and never against it: a modem repeats itself at other periods too (in
its preamble, in an idle pattern, or over several frames when its probes
vary). It is what tells some of the 2400 and 1200 Bd serial modems apart,
which share every other measured parameter:

| Mode | Frame | Provenance |
|---|---|---|
| STANAG 4285 | 106.67 ms (256 symbols, an 80-symbol preamble in each) | vendor (Wavecom) |
| STANAG 4529 | 213.33 ms (80 + 176 symbols at 1200 Bd) | vendor (Wavecom) |
| STANAG 4539 / 110 App C | 119.6 ms (256 data + 31 probe symbols) | observed (sigidwiki) |
| MIL-STD-188-110 serial | 66.67 ms (up to 1200 bps) or 200 ms (2400, 4800 bps): the probes repeat when the 160-symbol scrambler and the 40- or 48-symbol frames line up | observed (an analyst quoting the standard on the scrambler; derived here, and measured on the wiki's samples) |
| CIS BPSK 1200 | 426.67 ms (a 512-bit block) | observed (sigidwiki) |

How often the meter finds these on real recordings: `docs/RESEARCH.md`
section 3.7.

The military and government modems come from `docs/RESEARCH.md` section 3,
with the same provenance: only the parameters that table gives, nothing
added. Their sync patterns (more evidence for layer 2) are still only in
`docs/RESEARCH.md`. Modes whose tone spacing is not
published (CIS-112, CIS-20, Serdolik) match on their tone count and rate
only.
