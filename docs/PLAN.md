# Plan

What this program is, how it fits into SDRconnect, and the order it is
built in. The research behind it is in [RESEARCH.md](RESEARCH.md); this
document is where decisions are recorded.

## Scope

One program, installed next to SDRconnect and used only with it, with an
SDRplay RSPduo. It watches the IQ that SDRconnect streams, finds every
emission in it, and for each one:

- **decodes it** if it is a mode one of the existing decoders handles;
- **classifies it** otherwise: modulation, modem, measured parameters,
  confidence and the closest candidates, never a bare label. No payload
  decoding and no decryption for these.

Passive reception only.

| Decoded | Source of the decoder |
|---|---|
| NAVTEX, SITOR-B | `SDR_plugin_NAVTEX` (core: `sitorb_decoder`, `fec_decoder`, `ccir476`) |
| RTTY / Baudot FSK (45.45, 50, 75 Bd presets, any baud and shift) | `SDR_plugin_NAVTEX` (`rtty_decoder`, `ita2`) |
| ALE 2G (141A/B), 3G (BW0/BW1), 4G WALE (Deep/Fast) | `SDR_plugin_NAVTEX` (`ale*`) |
| Morse (CW), 5–50 WPM | `SDR_plugin_OOK` |
| PSK31, PSK63, PSK125, PSK250, PSK500 (BPSK and QPSK) | `SDR_plugin_PSK` |

Everything else in the catalogue is classified (RESEARCH.md, section 3):
the CIS/Russian modes, STANAG and MIL-STD modems, Link-11, HFDL, PACTOR,
DSC, SITOR-A, CODAN, Chinese modes, voice and others. A decoded mode is also
classified, so that the two can be compared.

The decoders keep their own validation: they come in with their tests and
their measured results. Their input changes from SDRuno's audio to audio
this program makes from the IQ (a digital down-converter and SSB demodulator
per emission), so their results are measured again on that path.

## How it fits into SDRconnect

SDRconnect has three interfaces to other programs. Only one carries the signal.

| Interface | What it does | Use here |
|---|---|---|
| **WebSocket API** (1.0.6+, API 1.0.3 in 1.0.9+) | IQ, audio and spectrum streams; reading and setting tuning, sample rate, gain, antenna, demodulator, filter; device selection incl. RSPduo dual-tuner mode; recording; device profiles. Works with the GUI and with `SDRconnect_headless` | **The integration.** Everything goes through it |
| Rig Control (1.0.5+, OmniRig / Hamlib) | Links SDRconnect to a *transceiver's* CAT port, so that SDRconnect follows the radio (a panadapter) and mutes on transmit | Not used: no signal data, and it is about an external radio |
| CAT control emulator (1.0.7+) | SDRconnect pretends to be a radio on a serial port, so that older programs can tune it. Which radio it emulates is not documented in anything found so far | Not used: control only, and the WebSocket API does the same and more |

"Most integrated" therefore means:

1. **One connection to SDRconnect's WebSocket server** (127.0.0.1:5454). If
   nothing answers and the SDRconnect window is not running, the program
   starts `SDRconnect_headless.exe --websocket_port=5454` itself and stops it
   at exit (to be confirmed on your installation; the option comes from a
   third-party guide). The window says what the connection is doing and
   what to do next.
2. **It follows SDRconnect.** When you retune SDRconnect, the program sees it
   (`property_changed`) and follows; it never fights you for the tuner you are
   driving. In dual-tuner mode it can drive the second tuner itself.
3. **Click to listen.** Selecting an emission in the program sets
   SDRconnect's VFO, demodulator (USB/LSB/AM/CW) and filter, so you hear it in
   SDRconnect.
4. **Recordings** with SDRconnect's own recorder (`start_recording iq`) or
   with this program (SigMF, with UTC and gaps marked; `sdrc_record`).
5. **Device profiles** you make in SDRconnect (`apply_device_profile`) for
   settings the API does not expose (IF bandwidth, IF AGC).
6. **Labels inside SDRconnect.** SDRconnect can draw its frequency list as
   markers on its spectrum. Whether that list can be filled from outside is
   unknown (no API for it; file format not documented). To be checked on your
   installation: export a list from SDRconnect and look at the file.

What the API does not give, and how the program copes (details in
`core/README.md`): IQ messages carry no time, rate or counter; refused
settings get no answer; only one client can control the receiver.

## Stack

C++17 core with no dependencies beyond the standard library and the
platform's sockets: the decoders it reuses are C++17, and the repositories
they come from already build, test and package that way (MSVC, CMake, NSIS).
Python only offline, to train models; models run in the program through
ONNX Runtime (milestone 6). SQLite for results. A local web interface served
by the program on 127.0.0.1, with every file bundled (works offline).
Settings are command-line options for now (a TOML file when there are more);
the catalogue is JSON, which is also valid YAML.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | SDRconnect connection: WebSocket client, IQ clock (sample numbering, UTC, gap detection), SigMF recorder, `sdrc_probe` to measure a setup, fake SDRconnect for tests | **done** (tested against the fake and the documented protocol only) |
| 2 | Wideband detector and emission tracker (noise floor, CFAR), SQLite, web interface with waterfall, emission lane, table, search, export; follow SDRconnect, click to listen | **done**. Tones and carriers that start together are grouped into one emission (a heuristic, see core/README.md); grouping by features is part of 4. Tested on synthetic signals and against the fake SDRconnect, one tuner and both |
| 3 | Decoders from the author's three SDRuno plugin repositories on per-emission audio from the IQ, and FT8/FT4 (ft8_lib) as the first ground truth for amateur signals | **done**: NAVTEX/SITOR-B, RTTY 45.45/170 and 50/450, CW, PSK31 to PSK500 and QPSK31 to QPSK500, ALE 2G/3G/4G, FT8, FT4. Tested on synthetic signals, and on the Signal Identification Wiki's samples (README) |
| 4 | Feature extraction, catalogue v0 (amateur part started: `catalogue/modes.json`), parametric rules (layer 1), grouping by features (shift, baud, carrier spacing), signal generators with fading, benchmark harness | **started** (2026-11-07): feature meter (tones, shift, symbol rate, envelope, modulation family) on every emission; catalogue v1 with `match` blocks for 49 modes, including the military and government modems of RESEARCH.md section 3; the parametric layer lists the modes an emission is consistent with; RTTY decoded at any measured rate and shift; `tools/sigidwiki_bench.py` measures all of it on the Signal Identification Wiki's samples. 2026-11-09: a weak wideband modem is one emission (the detector's running-median pass), and every emission's frame period is measured. 2026-11-10: the meter scored against the wiki's pages (FSK read as FSK 105 -> 149 of 247 samples, RTTY rates 0 -> 11 of 17); frequency lists as a prior (shown per emission, rank the catalogue, name decoders); GMDSS DSC decoded. Still to do: grouping by features, generators with fading, confusion matrices on your recordings |
| 5 | Sync/preamble evidence (layer 2): public preambles, ALE, DSC, T-600, Akula | **started** (2026-11-09): frame periods in the catalogue tell apart STANAG 4285, 4529 and 4539, MIL-STD-188-110 serial and CIS BPSK 1200 (on the wiki's samples, a right mode first for 41 of 49, `docs/RESEARCH.md` section 3.7). 2026-11-10: Akula's published sync and end groups, and 7-bit constant-weight codes (CIS-36-50 / CIS-50-50, the SITOR family), are searched for on FSK emissions (recognition, no traffic read). Still to do: the other sync patterns (T-600's start and end sequences, STANAG 4285's preamble) |
| 6 | Machine-learning layer (layer 3), "unknown / similar to", calibrated confidence | **started** (2026-11-10): a random forest over the feature meter's vector names one of seven families with a confidence, or "not sure"; trained and compared (forest, SVM, MLP, CNN, ResNet, transformer) on the wiki's samples with pages held out (`tools/ml/README.md`); embedded and run in C++. Still to do: real recordings and synthetic signals with fading as data, "similar to" for unknowns, system-level classes |
| 7 | Evaluation on real recordings: per-class precision/recall and confusion matrices | |

Done alongside milestone 3 (2026-11-06): a Windows installer (English and
Bulgarian), `HFClassifier.exe` in the notification area, settings in the
user's data folder with a Settings dialog, starting the headless SDRconnect
automatically, and a redesigned UI in Bulgarian and English. Not yet tried
on a real Windows PC with SDRconnect; CI installs and runs it on a Windows
build machine without SDRconnect.

Decoded text is the ground truth the later layers are measured against: an
emission the program decodes as FT8, NAVTEX or ALE was certainly that mode
(their decoders check codes or CRCs), so live reception labels training and
test data for the classifier without anyone labelling by hand. CW, PSK and
RTTY decodes are strong evidence but not proof: their text passes a
plausibility rule, not a check code.

A review of milestones 1 to 3 on 2026-11-01 (two code reviews, stress tests
with signals of other modes and an SNR sweep) found and fixed, among
others: false CW/PSK/RTTY decodes of other modes' signals, ALE missed below
about 15 dB, strong CW losing its start, an FT8 hang on busy bands, reader
stalls taken for lost IQ, and the two tuners of an RSPduo overwriting each
other in the log. Each now has a test.

## What is still needed

1. **`sdrc_probe` run on a real setup** (README): its `sdrc_probe_report.json`.
   The defaults (sample rates, processing load) depend on its answers: which sample rates arrive in
   full, what dual-tuner mode allows, the antenna names.
2. **Definitions** of CIS-75 FSK, CIS MFSK-16 (XPA2 or XPB?), 16FSK, "FSK 600
   Bd families", Polish FSK 100 Bd (F11 or POL-ARQ?), the Harris mode, CHN
   π/2-BPSK 2400, CHN MIL 8PSK 1627, Turkish Navy FSK 600 Bd.
3. **IQ recordings** (`sdrc_record`) of the modes in RESEARCH.md section 3.5,
   with frequency, date and what they are thought to be.
4. The frequency and date of the Akula FSK clip of RESEARCH.md section 3.6, and whether
   "7.918 MHz" in the 15-channel Akula file is the 24 kHz centre of that recording.
5. **`hf_classifier` tried with a real SDRconnect**, to find out:
   - whether *Listen in SDRconnect* puts a USB signal in the middle of the
     passband. If it sounds about 1.5 kHz off, SDRconnect's VFO is not the
     dial frequency, and the assumption in `app/README.md` must change;
   - whether the decodes and emissions look right on a busy band, e.g.
     the FT8 and NAVTEX frequencies;
   - how much CPU it takes at the sample rate you use;
   - whether `SDRconnect_headless --websocket_port=5454` works (the option
     comes from third-party guides only).
