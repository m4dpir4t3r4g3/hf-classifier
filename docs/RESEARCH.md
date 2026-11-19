# HF modem classifier for SDRconnect + RSPduo — Step 0 research report

Status: research for Step 0, approved on 2026-10-19 with two changes of scope:
modes your existing decoders handle are **decoded**; everything else is
**classified**. The plan that follows from it is in [PLAN.md](PLAN.md); where
this report and the plan disagree, the plan wins.
Date: 2026-10-19 (UTC). Sources are numbered `[n]` and listed at the end with licences.

## How to read the provenance tags

Every parameter in this report carries one tag. When a number comes from a summary of a
standard (for example Wavecom's page about STANAG 4285) and I did not read the standard itself
in this session, it is tagged **V**, not **S**.

| Tag | Meaning |
|---|---|
| **S** | Read in a primary specification in this session (SDRplay documents, or standards already implemented from their text in the author's `SDR_plugin_NAVTEX` ALE decoder) |
| **S?** | From a primary standard, but quoted from memory and **not re-read** in this session. Must be verified before use |
| **V** | Vendor documentation (Wavecom decoder manual, Harris brochure) |
| **O** | Observations by analysts or hobbyists (sigidwiki, Priyom, the i56578 blog, radioscanner.ru). Not documented by whoever transmits the signal |
| **M** | Third-party measurement (for example probe results against a real SDRconnect) |
| **E** | Measured in this project on **recordings supplied for it** (section 3.6). The method is stated there |
| **G** | My engineering estimate. Not measured. Must be tested |
| **—** | Nothing public found |

None of the Russian/CIS or Chinese modes has a public specification from its originator. Every
CIS parameter below is an **O** at best.

---

## 0. Summary

1. **SDRconnect exposes raw IQ to other programs.** It does this through an official WebSocket
   API on port 5454 (API 1.0.3, SDRconnect ≥ 1.0.9; 1.0.10 is current). It works with the GUI
   and with the headless binary. IQ arrives as binary messages: little-endian signed 16-bit
   interleaved I/Q. Type 2 is the primary tuner and type 5 the secondary [1][3].
   - Frequency, sample rate, LNA state and antenna are set with `set_property`.
   - IQ messages carry **no timestamp, no sample rate and no counter**. The tool must track all
     three itself.
2. **IQ comes at the full device sample rate, not a VFO slice.** A third party measured this on
   an RSP1A; SDRplay does not document it [5][6].
   - 62.5 kS/s to 10 MS/s were accepted. 3 MS/s was silently refused.
   - Delivery was within 1 % from 250 kS/s up to 2 MS/s. Rates above 2 MS/s were accepted but
     their delivery was not measured.
   - If the reader stalls, SDRconnect buffers about 2.5 s of IQ, then drops data without
     disconnecting.
3. **The RSPduo has two useful modes in SDRconnect: single tuner and dual tuner [8][2][3].**
   - **Single tuner:** up to 10 MHz visible bandwidth. The ADC is 14-bit up to 6.048 MS/s, then
     12-, 10- and 8-bit at higher rates [9].
   - **Dual tuner:** two independent streams, each low-IF and **at most 1.536 MHz** of usable
     bandwidth ("2 slices of 2 MHz") [9][10].
   - The Hi-Z port (1 kHz–30 MHz) exists only on tuner 1 [9].
   - Master/slave is an SDRplay API mode [10]. SDRconnect does not document it, so I do not plan
     on it.
4. **Your example split ("A = wide survey, B = follow an emission") is only partly possible.**
   In dual-tuner mode neither tuner can be wide; both are limited to about 1.5 MHz.
   - **Option W (wide):** one tuner at 6–8 MS/s, about 5–7 MHz of HF at once. "Following" an
     emission is then done digitally inside that window.
   - **Option D (dual):** two independent windows of about 1.5 MHz each. B can retune to dwell on
     an emission outside A's window.
   - I recommend supporting both, with W as the default. Section 2 has the details and the
     fallback.
5. **There is no public labelled IQ dataset of CIS/NATO HF modems with a clean licence.**
   - The closest prior art is Scholl 2025: 160 HF classes including many military modes, a CNN
     on 4 kHz IQ, about 90 % top-1 at high SNR. Its dataset release and licence are not stated
     [27].
   - sigidwiki audio grants no reuse licence [12], and most of it is MP3, which damages phase.
   - Training and evaluation data must therefore come from our own synthetic generators and your
     recordings.
6. **Several catalogue entries can't be specified from public sources**, so I can't build honest
   generators for them. These are: CIS-75 FSK, "16FSK", "FSK 600 Bd families", Turkish Navy FSK
   600 Bd, CHN π/2-BPSK 2400, CHN MIL 8PSK 1627, the Harris RF-5800 proprietary waveform, and
   the details of the Serdolik v2 OFDM. I need your source or definition, and recordings
   (section 3.5).
7. **Some pairs can't be told apart from the waveform alone.** These will be reported as joint
   classes, or with an explicit frequency-database hint:
   - STANAG 4415 is on air identical to MIL-STD-188-110A at 75 bps [11].
   - DSC-HF and CODAN Selcal use the same M.493 alphabet with the same baud rate and shift
     [11].
   - "Harris RF-5800" is a radio, not a waveform. It mostly transmits standard waveforms [19].

---

## 1. SDRconnect integration

### 1.1 What is documented (SDRplay sources)

**Transport, versions and setup**

| Item | Fact | Src |
|---|---|---|
| Transport | WebSocket. The same spec applies to SDRconnect GUI and SDRconnect Headless [1]. Port 5454: from SDRplay's example clients [3] (the API document names no port) | S [1][3] |
| API history | Added in 1.0.6 (Jan 2026). 1.0.7 added the experimental headless binary, open by serial or name, and made `device_sample_rate` read-write. 1.0.8 added RSPduo dual-tuner support (API 1.0.2). 1.0.9 added the read-only `api_version` (1.0.3). 1.0.10 (17 Aug 2026) added WSS and access control by config file, both **headless only** | S [2] |
| Current version | SDRconnect 1.0.10. Supports all RSPs except RSP1. 64-bit only | S [4] |
| Enabling | Off by default. Turned on in Preferences → WebSocket Server (another third-party guide: "⋮ → Settings → WebSocket"). Not stated by SDRplay | M [5][6] |
| Network exposure | When enabled it listens on **0.0.0.0:5454**, so it is reachable from the LAN. Windows Firewall allows it on the Private profile | M [5] |

**Messages**

| Item | Fact | Src |
|---|---|---|
| Text messages | JSON `{event_type, property, value, device}`. `device` is `primary` (the default) or `secondary` | S [1] |
| Client → server events | `set_property`, `get_property`, `iq_stream_enable`, `audio_stream_enable`, `spectrum_enable`, `device_stream_enable`, `selected_device` / `_serial` / `_name`, `start_recording` / `stop_recording` (`iq`, `audio`, `compressed_audio`), `apply_device_profile`, `set_primary_device_enable`, `set_secondary_device_enable` | S [1] |
| Server → client events | `property_changed` (push) and `get_property_response` | S [1] |
| Binary messages | A 2-byte LE type, then the payload. Types 1/4: 48 kHz stereo S16 audio (primary/secondary). **Types 2/5: S16 interleaved IQ (primary/secondary).** Types 3/6: U8 FFT bins scaled to the visible range | S [1][3] |

**Properties**

| Item | Fact | Src |
|---|---|---|
| Tuning and rate | `device_center_frequency` (LO, Hz), `device_sample_rate` (Hz, read-write since 1.0.7), `device_vfo_frequency`, `filter_bandwidth`, `demodulator` | S [1] |
| Gain and ports | `lna_state` with read-only `lna_state_min/max`, `valid_antennas` (read-only list) and `active_antenna`. **No IF-gain, IF-AGC, IF-bandwidth, IF-mode or decimation property exists** | S [1] |
| Status | `started`, `overload`, `can_control` (all read-only), `signal_power`, `signal_snr` (VFO), `api_version` | S [1] |
| Selecting dual-tuner mode | SDRplay's own `MinimalDuoExample` selects the device with `"<serial>:Dual"`, enables primary and secondary, enables IQ, and tunes the two LOs independently (10 MHz and 50 MHz in the example) | S [3] |

**Checked again on 2026-11-01** against SDRplay's API document (same file
as before), the 1.0.10 release notes and SDRplay's C# and JavaScript
examples: no contradiction with the above. Additions: 1.0.10 also adds a
2.048 MS/s rate ("for DAB"); SDRplay's own client reads the `device` field
case-insensitively (this program does too since then); the dual-tuner
selection `"<serial>:Dual"` is shown only in SDRplay's MinimalDuoExample,
not in the API document; `device_vfo_frequency` is documented only as "VFO
tuned frequency" (whether it is the dial in SSB is not stated), and an
`ssb_lowcut_frequency` property exists that this program does not set; the
headless `--websocket_port` option appears in no SDRplay document (third-
party guides only). SDRplay's forum could not be reached (HTTP 502).

### 1.2 Measured by third parties (RSP1A, Windows 10, API 1.0.3) [5][6]

These measurements come from an open-source project's probe (pdn-soundmodem, AGPL-3).
They are evidence, not documentation.

**Sample rates**
- Accepted and reported back: 62.5k, 125k, 250k, 500k, 1M, 2M, 6M, 8M and 10M S/s.
- **3 MS/s was refused with no error.**
- A rate change is reported about 0.57 s after the request.

**Delivery**
- IQ arrives at the reported device rate, within 1 % from 250 kS/s to 2 MS/s.
- 62.5k and 125k came in about 3 % short.
- 6, 8 and 10 MS/s were accepted, **but their delivered rate was not measured.**
- There are about 20 IQ messages per second. 1 MS/s is 200 032 bytes per message, about
  32 Mbit/s; 2 MS/s is about 64 Mbit/s.

**Signal levels**
- Samples use the full 16-bit scale. There is no meaningful DC offset.
- 40 m noise measured −43.8 dBFS rms.
- **SDRconnect's audio AGC does not touch the IQ.**

**Back-pressure**
- A stalled reader gets about 2.5 s of buffering, then data is dropped and the socket stays
  open.
- A dedicated reader thread is therefore mandatory, with gap detection by counting samples
  against the clock.

**Retuning**
- After a retune the client discarded 100 ms of IQ. IQ frames carry no tuning metadata, so the
  tool must gate on the `property_changed` event plus a guard time.

### 1.3 Unknowns, to be measured on an RSPduo before any DSP work

The first deliverable of Step 1 is a small **probe tool** that measures these on the user's PC.

1. The accepted `device_sample_rate` values for the RSPduo in single-tuner and in dual-tuner
   mode. Can dual mode go below 2 MS/s (decimated)?
2. Whether 6, 8 and 10 MS/s are delivered in full over the WebSocket on your CPU. 8 MS/s of S16
   IQ is 256 Mbit/s over loopback.
3. The `valid_antennas` names on the RSPduo (Tuner 1 50 Ω, Tuner 1 Hi-Z, Tuner 2), and whether
   Hi-Z can be selected per tuner in dual mode.
4. Which IF bandwidth SDRconnect picks for each sample rate, and whether IF AGC is active. The
   API exposes neither. Both matter for absolute level and SNR measurements and for CFAR.
5. The exact device-selection strings for single-tuner mode and for choosing tuner 1 or tuner 2
   (only `:Dual` is shown in an example).
6. Whether both IQ streams (types 2 and 5) can be enabled at once at full rate.
7. How headless is started (command-line options are not in the documents I found), and what
   format the 1.0.10 access-control config file uses.
8. `can_control` behaviour when the SDRconnect GUI is also connected. With `can_control=false`
   the tool has to run read-only.

### 1.4 Consequences for the design

- **Timestamps.** The tool assigns UTC as the host clock at the first sample, plus the sample
  count divided by the measured rate. It re-anchors after every gap or retune and keeps the
  host-clock jitter. Absolute UTC accuracy will be about tens of ms; this is a guess (**G**).
  Burst timing inside one emission uses sample counts, so it is exact.
- **Security.** The tool connects to 127.0.0.1 only. The README will tell you to block 5454
  from the LAN (Windows Firewall, or headless access control in 1.0.10).
- **No other route is needed.** IQ comes from the WebSocket. Control also goes through it, or
  through device profiles (`apply_device_profile`) for settings the API does not expose.

---

## 2. RSPduo specifics

### 2.1 Hardware facts [9][10]

| Item | Value | Src |
|---|---|---|
| Coverage | 1 kHz – 2 GHz, both tuners | S |
| Ports | Tuner 1: 50 Ω SMA (1 kHz–2 GHz) **and Hi-Z** (1 kΩ balanced, 1 kHz–30 MHz, choice of 0–2 MHz or 0–30 MHz low-pass, MW notch). Tuner 2: 50 Ω SMA, with a Bias-T (4.7 V, 100 mA) | S |
| HF preselection (50 Ω) | LPF 2 MHz, then BPF 2–12 MHz, 12–30 MHz, … | S |
| ADC | 2–10.66 MS/s. 14-bit native from 2 to 6.048 MS/s, 12-bit from 6.048 to 8.064, 10-bit from 8.064 to 9.216, 8-bit above 9.216 | S |
| IF modes | Zero-IF supports all IF bandwidths (200 k, 300 k, 600 k, 1.536 M, 5, 6, 7, 8 MHz). **Low-IF supports IF bandwidths ≤ 1.536 MHz only** | S |
| Operating modes (API) | Single tuner (one stream, any IF mode or bandwidth), Dual tuner (both streams in one instance, low-IF, ≤ 1.536 MHz), Master/Slave (two applications; the master sets the shared parameters; 6 or 8 MHz) | S |
| LNA states | API constants: 10 in general, 7 in the "AM band", 5 on the Hi-Z port. SDRconnect exposes the range through `lna_state_min/max`, so the tool reads it at run time | S |
| Noise figure (50 Ω) | 18 dB at 2 MHz, 15 dB at 12–40 MHz. Hi-Z: 14 dB at 2 MHz, 18 dB at 15 MHz | S |
| Reference | 0.5 ppm TCXO. External 24 MHz reference in and out | S |

### 2.2 What SDRconnect supports

- **Supported:** single-tuner and dual-tuner mode in one SDRconnect instance [8][2]. The
  WebSocket API has supported dual tuner since 1.0.2 [2], with secondary IQ as binary type 5
  [1][3].
- **Diversity:** supported (manual diversity in 2023; below 2 MS/s since 1.0.9) [8][2]. It
  combines two antennas into **one** stream, so it is not two inputs.
- **Not documented in SDRconnect:** master/slave.

### 2.3 Can both tuners be two independent inputs?

**Yes, but only as two narrow (≤ 1.536 MHz) windows.**
- A wide survey and a narrow follower cannot run at the same time.
- In dual-tuner mode the ADC runs at 6 or 8 MHz with low-IF and 1.536 MHz filters [10]. From
  the datasheet table, that means 14-bit at 6 MHz and 12-bit at 8 MHz. This is an inference, and
  the probe must confirm what SDRconnect uses.

### 2.4 Proposed input modes

| Mode | Configuration | What you get | Use |
|---|---|---|---|
| **W — Wide (default)** | Single tuner, 6 MS/s (14-bit) or 8 MS/s (12-bit), if the probe confirms full delivery | One window of about 5–7 MHz (5 or 8 MHz IF filter; **G** until the probe) | Survey a whole band segment, e.g. 6.2–11.2 MHz. Every emission inside is channelised digitally, so no second tuner is needed to "follow" |
| **D — Dual** | Dual tuner, 2 × 2 MS/s | Two independent windows of about 1.5 MHz anywhere in HF. Hi-Z only on A (tuner 1) | A surveys one band and B surveys another; or B is retuned by the tool to dwell on an emission outside A, or to watch a scheduled frequency |
| **Fallback** | If dual-tuner IQ is unavailable or unreliable over the WebSocket on a given setup, use W only | — | "Follow" is purely digital inside W. A second input can still be a file-playback source (for development) |

Also supported in both modes: **SigMF recording** (whole window, or per-emission pre-roll from the
ring buffer) and **file playback** (SigMF; SDRconnect/SDRuno IQ WAV), so that development and
testing can run offline.

---

## 3. Modem catalogue: public parameters, sync evidence, confusability

**Conventions in the tables**
- **Audio-domain numbers are relative to the transmitter's suppressed carrier (the dial
  frequency).** In RF IQ we never see the dial frequency, so such numbers become *relative
  constraints*. For example, "1800 Hz centre" means dial = observed centre − 1800 Hz, and "pilot
  +3300 Hz" means the pilot sits 3300 Hz above the dial. An LSB transmission mirrors every tone
  plan, so the classifier tests both polarities.
- **"Sep." is the expected separability from the nearest confusable mode, and it is tagged G.**
  It is my engineering estimate, to be replaced by measured confusion matrices.

### 3.1 Russian / CIS

| Class | Family | Nominal parameters | Sync / structure usable as evidence | Confusable with | Sep. (G) |
|---|---|---|---|---|---|
| **CIS-36-50** (T-600, BEE-36) | FSK | **O** [13]: HF 50 Bd/200 Hz, 50 Bd/250 Hz, 100 Bd/500 Hz (VLF 50 Bd/75 Hz). **V** [11]: 36, 50, 75, 100, 150 Bd; shift 85, 125, 250, 500 Hz; a 36 Bd idle sequence, then 50 Bd traffic. **O** [36]: a **50 Bd / 1000 Hz** variant, seen just before an Akula burst on 9202 kHz CF (2025). 5-bit characters coded as 7-bit constant-weight words, 4:3 ones to zeros (3:4 in the other polarity) [13][37][38]. Rivet also handles 75 and 400 Hz shifts [38] | **O** [13]: reversals, then one of three published **44-bit SOM sequences**, a 70-bit IV/session key sent twice, and an EOM of 5 × `0001000`. **Cross-checked:** Priyom's ship-to-shore SOM is `11` followed by i56578's 42-bit start sequence [37]. The SOM deliberately breaks the 4:3 weight (23 ones in 44 bits) [37]. Rivet does the same check in the inverse polarity (21 of 44) and finds the EOM as `1110111`, which is the inverse of `0001000` [38]. i56578's "000100" EOM word is 6 characters, so it is a typo [36][37]. **Structural statistic:** the share of 7-bit words with 4:3 weight (97.5 % measured by i56578 [36]) | CIS-50-50; RTTY 50 Bd; Polish F11; SITOR (also a 7-bit 4:3 code, but 100 Bd / 170 Hz) | High against others. **Medium against CIS-50-50** |
| **CIS-50-50** | FSK | **V/O** [11][12]: 50 Bd (rarely 100); shift 85/125/200/250/500 Hz; idle dot reversals at 50 Bd | Same alphabet and message structure per Wavecom [11]. Priyom treats both as one system, T-600 [13] | CIS-36-50 | Medium (idle rate 36 vs 50 Bd; shift set) |
| **CIS-75 FSK** | FSK | **—** No public description found. Wavecom lists 75 Bd only as one of the CIS-36-50 rates [11] | — | — | Needs your definition and recordings |
| **CIS-12** (PSK2 / PSK4 sub-classes) | Multitone PSK | **V** [11]: 12 subcarriers, 200 Hz apart, 120 Bd each, **DBPSK or DQPSK**, unmodulated pilot at **+3300 Hz** from the suppressed carrier. **O** [12]: up to 4800 bps; AT-3004D/AT-3104D; an alternate mode exists | Pilot at +3300 Hz; idle pattern; a "standby" mode exists [12] | CIS-20, CIS-16, Link-11 CLEW, 110 App A 16-tone, Codan 9001 | High for CIS-12 against the others. **Medium for PSK2 against PSK4** (needs ≳10 dB per subcarrier, G) |
| **CIS BPSK 1200** (CIS-1200, T-230-1A "Makhovik") | Single-carrier PSK | **O** [12][17][18]: 1200 Bd, BPSK and SDPSK variants, about 2.5 kHz. A 512-bit block = **426.67 ms** period. Discussion of 511-bit m-sequence sync | 426.67 ms periodicity; m-sequence sync (O, unconfirmed) | STANAG 4529 (1200 Bd, 213.33 ms frame); CIS-1280 (OQPSK) | Medium–High |
| **CIS MFSK-16** | MFSK | **Ambiguous.** XPA2: 14 used of 16 tone slots, **15 Hz spacing, 7.5 Bd, ~250 Hz**, a sync tone every 5 symbols (**O** [12]). XPB: 16 tones, ~2.8 kHz wide (**O** [12]). Not the amateur MFSK16 (16 tones, 15.625 Hz, 15.625 Bd, **V** [11]) | XPA2's long preamble and periodic low sync tone [12] | Amateur MFSK16; XPA; Serdolik command state | High if the spacing is measured to ≲0.3 Hz (several seconds of signal). **Which variant do you mean?** |
| **CIS-112** | OFDM | **O** [12]: 112 tones, DQPSK at **22.22 Bd**; preamble of 8 tones (incl. carrier), then a 56-tone OFDM preamble, then 112-tone data; pilot +3300 Hz. Tone spacing not published | Fixed 8-tone → 56-tone → 112-tone sequence | CIS-128, CIS OFDM-121, CIS OFDM HDR (45/60/93 tones) | Medium (tone counting at 22 Bd needs ≥ 1 s FFTs) |
| **CIS-3000** | Single-carrier 8PSK | **O** [12]: 8PSK at **3000 Bd**, ~3.4 kHz; seen as a calling mode before CIS-128 or MFSK-68; data reportedly in Proesch's *Technical Handbook for Radio Monitoring HF* (a commercial book I don't have) | — | 2400 Bd serial modems | High (baud) |
| **CIS-20** | OFDM | **O** [12]: 20 tones, DQPSK, **75 Bd** each; pilot +3300 Hz; ~2.75 kHz; AT-3104 | Pilot | CIS-12, CIS-16, Codan 9001 (16 × 75 Bd), Chinese 4+4 (8 × 75 Bd), CLEW | High (tone count) |
| **Akula** | FSK burst (+ multi-carrier DBPSK) | **E** [41] (section 3.6): 500 Bd FSK, shift 999.9 Hz, sync and EOM+EOT found with 0 errors. **New:** a **15-carrier DBPSK** form (500 Bd per carrier, carriers exactly 2000 Hz apart, about 30 kHz in total). It carries a stream **bit-identical** to the FSK transmission that follows it. **O** [15][35][36]: **500 Bd, 1000 Hz shift**, short bursts (the legacy format was ten 5-figure groups, 0.72 s on air); **6-bit words**. Variants: **Akula II** = a short **BPSK 500 Bd** burst, sometimes sent before the FSK traffic; a **100 Bd / 1000 Hz** variant (2020); "data-only" bursts with no sync, preamble or IV but with the normal EOM+EOT (2024, 2025). **Seen in your screenshots [39]:** Akula II at **500.02 Bd**, carrier 1500.57 Hz (13.404 MHz, 2015-07-22). The 100 Bd variant measured **Br 100.1, dF 996.43 Hz** (12478 kHz USB, 2020-08-27). A 500.0 Bd 2-FSK burst (10.6348 MHz, 2018-03-26). A KiwiSDR recording from i56578 exists [40] (USB audio, 9201 kHz, 2025-04-16), but it could not be downloaded here (Yandex storage times out). Logged centre frequencies 3399–16264 kHz, with 9201/9202 kHz active [15][36] | Raw demodulated bits, 6-bit framing [35]: **sync group** 4 × `100101` + 2 × `110001`, then a `000000` separator. **Preamble group:** 7 words (4 × word A + 3 × word B; A and B not published). **EOM+EOT:** `101111 100010 100010 101111 011110`, also visible in your bitmaps [39]. Also: after descrambling with x⁵+x³+x+1 (or x⁴+x³+1 after differential decoding) the stream shows a **6-bit period** [15]. **Detection must not require every group**, because data-only bursts exist [35][36] | F06 (200 Bd/1000 Hz), F01 (200 Bd/500 Hz); CIS-Navy 50 Bd/1000 Hz (same 1000 Hz shift); 100 Bd FSK modes | High (baud, burst length, EOM+EOT) |
| **Serdolik v2** (2018) | MFSK + OFDM | **O** [14]: link set-up = a two-tone address sequence while scanning 3 duplex channels (30 s each). Command state = **35-tone MFSK at 40 Bd**. Traffic at 40 Bd: MFSK 200–1200 bit/s (1/2/4/7/15 tones per symbol); OFDM 2400 (60 × BPSK), 3600 (45 × QPSK), 4800 (60 × QPSK), 7200 (60 × 8PSK). **Tone spacing not published.** **O** [16]: a 35-tone DQPSK OFDM with cyclic prefix | Two-tone call; 35-tone command block | CIS-36 (CROWD36: 36 tones, 40 Hz, 10/20/40 Bd, **V** [11]); old Serdolik MFSK-34; CIS OFDM HDR | Medium (needs recordings). Note: sigidwiki's Serdolik page mixes it with CROWD36 and is flagged "Rework" [12] |
| **Morse stations** | OOK (A1A); also FSK-CW | **O** [12]: about 4 Hz bandwidth per WPM. FSK-CW keeps the carrier on and shifts it down in the gaps | Keying envelope statistics (dot/dash ratio, WPM). **No text decoding** | Carriers; slow FSK | High |

### 3.2 NATO / Western

| Class | Family | Nominal parameters | Sync / structure | Confusable with | Sep. (G) |
|---|---|---|---|---|---|
| **STANAG 4285** | Serial PSK | **V** [11]: 2400 Bd at 1800 Hz; 8PSK/QPSK/BPSK, 75–3600 bps. Frame = 256 symbols = **106.67 ms** (80-symbol preamble, then 32 data / 16 probe ×3 / 32 data). EOM `4B65A5B2` | 80-symbol preamble repeated in **every frame**. **S?**: BPSK from a 5-stage LFSR (period 31), to verify against the standard | 110A, 4539, 3G/4G ALE, Link-11 SLEW, Codan 3212 | High (106.67 ms periodicity; prior art [22]) |
| **STANAG 4529** | Serial PSK | **V** [11]: **1200 Bd**; centre selectable 800–2400 Hz in 100 Hz steps (default 1700); 1240 Hz bandwidth; frame **213.33 ms** (80 + 176 symbols); 75–1800 bps | Per-frame preamble | CIS-1200 | High |
| **STANAG 4539** (= 110B/C App C) | Serial PSK/QAM | **V** [11]: 2400 Bd at 1800 Hz; QPSK, 8PSK, 16/32/64-QAM; 3200–9600 bps (12 800 uncoded); 287-symbol preamble; 256 data + 31 probe symbols = **119.6 ms** (**O** [12]); 6 interleavers | Preamble encodes rate and interleaver | 110A, 4285, Codan 3212 | High |
| **STANAG 4415** | Serial PSK | **V** [11]: **on air identical to 110A at 75 bps** (Walsh-coded 8PSK at 2400 Bd, 1800 Hz). The difference is the receiver's tone excision | Same as 110A at 75 bps | **110A at 75 bps — indistinguishable** | Reported as the joint class "110A-75 / 4415" |
| **MIL-STD-188-110 serial** (110A/B/C) | Serial PSK | **V** [11]: 2400 Bd 8PSK at 1800 Hz; preamble 0.6 s (zero/short interleaver) or 4.8 s (long), built from 200 ms segments; data/probe 32/16 (≥ 2400 bps) or 20/20 (≤ 1200 bps); 75–4800 bps. Variant **110A-MOD**: 267 ms preamble, 20/20 frames (**V**) | Preamble with D1/D2 (rate and interleaver); prior art reads them [22] | 4285, 4539, 4415, 3G ALE | High |
| *(related)* 110 App A 16-tone / App B 39-tone | Multitone | **O** [12]: App A = 16 tones, 75 Bd, DQPSK/BPSK, 605 Hz Doppler pilot, 605 + 1705 Hz preamble tones. App B = 39 tones, **44.44 Bd**, pilot at ~393 Hz; STANAG 4197 is a close variant. **S?**: 56.25 Hz spacing, to verify | Preamble tones | **App A against Link-11 CLEW**; App B against 4197 | App A vs CLEW **Low–Medium**. App B vs 4197 Low |
| **ALE 2G** (141A/B App A) | 8-FSK | **S** [20]: 8 tones, 250 Hz apart, 750–2500 Hz, **125 Bd**; 49-tone words of **392 ms** | Your repo already detects word sync | Thales/Saab MFSK modems | High |
| **ALE 3G** (141B App C / STANAG 4538) | Burst 8PSK | **V** [11] / **S** [20]: 2400 Bd at 1800 Hz. Bursts: BW0 613.33 ms (384-symbol preamble), BW1 1306.67 ms (576), BW2 640 + 400n ms, BW3 373.33 + 13.33n ms, BW4 640 ms | BW0/BW1 preamble correlators already exist in `SDR_plugin_NAVTEX` | 110A, 4G | High for BW0/BW1, Medium for BW2–4 |
| **ALE 4G / WALE** (141D App G) | Burst 8PSK | **S** [20]: 2400 Bd; Deep WALE (fixed preamble part of 14 Walsh symbols) and Fast WALE (5 symbols); 96-bit PDUs. **O** [12]: it negotiates a **wideband 110D App D** channel of up to 48 kHz. That is a useful catalogue extension, since wideband IQ can see it | Correlators already exist in your repo | 3G ALE, 110A | High |
| **HFDL** | Burst PSK | **O** [12][21]: **1800 Bd on a 1440 Hz subcarrier**; BPSK/QPSK/8PSK at 300/600/1200/1800 bps; TDMA single- and double-slot frames. The ARINC 635 spec is commercial, and I did not read it | Burst/slot timing. Frequency prior: published ground-station table (dumphfdl `systable.conf`) [21] | Codan 3212 (1800 Bd) | High |
| **Link-11 CLEW** | 16-tone DQPSK | **V** [11]: Doppler tone **605 Hz**; data tones 935–2365 Hz every **110 Hz**, plus **2915 Hz** (data/sync); **75 or 45.45 Bd**; SSB or ISB (both sidebands). MIL-STD-188-203-1A / STANAG 5511 | Doppler tone, 2915 Hz tone, frame formats | **110 App A**, CIS-12/16/20 | Medium (needs recordings) |
| **Link-11 SLEW** | Serial PSK | **V** [11]: 1800 Hz, 2400 Bd, **QPSK**. **O** [12] says 8PSK (110 serial). **The sources disagree** | — | 110A serial | Low–Medium until resolved |
| **Harris RF-5800** | (a radio) | **V** [19] (search summary, not read in full): the radio transmits standard waveforms (110A/B incl. App C, 39-tone, 4285, 4415, 4539) plus a **proprietary ECCM serial-tone waveform** (75–2400 bps) whose parameters are not public | — | Every standard modem it carries | Standard waveforms are labelled as the standard. The proprietary mode needs recordings. **The radio model can't be inferred from the waveform** |
| **CODAN Selcal** | FSK | **V** [11]: **100 Bd**, mark 1870 / space 1700 Hz, ITU-R M.493 alphabet, ≥ 2 s of dot reversals, phasing of characters 125/108 alternating for 1.2 s, 10-bit characters | Dot pattern and phasing | **DSC-HF** (same family) | **Low** without a frequency prior |
| **CODAN 3212** | Serial PSK/QAM | **V** [11]: modified STANAG 4539, **1800 Bd centred on 1500 Hz**, 8PSK and 16/32/64-QAM, 75–7200 bps. Codan ALE "chirp" = **32 carriers, DBPSK at 80 Bd, 80 Hz spacing, 2560 Hz**. Codan 9001 = **16 tones at 75 Bd DQPSK, 112.5 Hz spacing** | Chirp frame: 23-bit preamble and 32-bit frame indicator (**V**) | HFDL (1800 Bd), 4539 (2400 Bd) | High against 4539; Medium against HFDL |
| **FSK 600 Bd families** | FSK | **—** Not defined publicly. Possibly STANAG 4481 FSK (**O** [12] lists rates up to 600 Bd, unconfirmed) | — | — | **Needs your definition** |
| **16FSK** | MFSK | **—** No HF military "16FSK" found | — | — | **Needs your definition** |
| **DSC-HF** | FSK | **V** [11]: **100 Bd, 170 Hz**, 10-bit symbols (7 information + 3 check), ITU-R M.493. **S?**: dot pattern, then phasing with DX = 125 and RX = 111…104 | Dot pattern and phasing | CODAN Selcal; SITOR | High with the DSC-channel prior; Low without it |
| **SITOR-A** | FSK ARQ | **V** [11]: 100 Bd, 170 Hz (also seen with 300/400/850 Hz, **O** [12]); **450 ms cycle**: 210 ms ISS block of 21 bits, then a 70 ms IRS ack | Burst timing | PACTOR-I, DSC | High (timing) |
| **SITOR-B** | FSK FEC | **V** [11]: 100 Bd, 170 Hz, continuous; each character sent twice, 35 bits apart | Continuous; repetition structure | DSC, NAVTEX | High (your repo already has the receiver) |
| **PACTOR I** | FSK ARQ | **V/O** [11][12]: 100/200 Bd, **200 Hz** shift; **1.25 s** cycle with 0.96 s packets | Cycle timing | SITOR-A | High |
| **PACTOR II** | 2-tone DPSK | **V** [11]: two DPSK channels, 100 Bd; DBPSK…D16PSK; ~450–500 Hz; 1.25 s or 3.75 s cycles; set up with PACTOR-I | Cycle timing; 2 tones | PACTOR III (SL1–2) | High |
| **PACTOR III** | Multitone DPSK | **V** [11]: 2–18 tones, **120 Hz** spacing, 100 Bd, DBPSK/DQPSK, up to 2.16 kHz; 1.25/3.75 s cycles | Speed-level tone sets (V [11] table) | PACTOR II, CIS multitone | High |
| **Voice USB / LSB / AM** | Analogue | No specification needed. Features: carrier line (AM), sideband asymmetry and the sharp edge at the suppressed carrier (SSB), syllabic envelope (3–5 Hz), pitch harmonics | — | Digital voice (looks like a data modem); multitone data | High for AM vs SSB; Medium–High for USB vs LSB (G) |

### 3.3 Other

| Class | Nominal parameters | Notes | Sep. (G) |
|---|---|---|---|
| **CHN π/2-BPSK 2400** | **—** Nothing public found. A possibly related post on a "Chinese PSK2 2400 Bd serial waveform" (i56578, Dec 2021) was not readable | Theory (G): π/2-BPSK has an x⁴ carrier line but, unlike BPSK, **no x² line at the carrier**; its x² lines are offset by ±Rs/4. That is a clean discriminator, if this mode really is π/2-BPSK | Needs recordings |
| **CHN MIL 8PSK 1627** | **—** Nothing public found | If the symbol rate is really 1627 Bd, the baud alone separates it | Needs recordings and your source |
| **Polish FSK 100 Bd** | **O** [12]: F11 = 100 Bd, **625 Hz** shift, periodicities of 960 ms and 160 ms; **inactive since 2018**. Separately, POL-ARQ = duplex FSK at 100/150/200 Bd, SITOR alphabet (**V** [11]) | Which one do you mean? | High (shift) |
| **Turkish Navy FSK 600 Bd** | **—** Nothing public found | — | Needs recordings |

Useful related entries that aren't in your list but have public descriptions (**V/O**). I suggest
adding them as "known non-targets" to reduce false labels: CIS-11, CIS-14, CIS-16, CIS-128,
CIS-1280, CIS OFDM-121/HDR, CIS-36 (CROWD36), F01/F06, Chinese 4+4 (8 × 75 Bd DQPSK, 300/450 Hz
spacing, **V**), STANAG 4481, STANAG 4197, ALIS, Thales Système 3000, Codan 9001 and chirp,
amateur modes (PSK31, RTTY, MFSK16, FT8), JORN and other OTH radars.

### 3.4 Existing classifiers, decoders and datasets

**Closed tools**
- Wavecom W-Code/W-Spectra, go2MONITOR, R&S CA100 and Hoka Code300-32. Their manuals are used
  here only as parameter references [11].

**Open-source references.** I'll use them as references only, and copy no GPL/AGPL code into the
core.

| Project | Licence | Relevance |
|---|---|---|
| sdrmm "STANAG modem identifier" [22] | AGPL-3.0 | Identifies 4285, 4529, 110A (incl. D1/D2), 4539, 39-tone, 16-tone and 2G ALE from **frame periodicity by FFT autocorrelation**. It passes 49 sigidwiki samples, but 3G ALE samples still get stray names, and STANAG 4481 isn't identified. **This is the best prior art for layer 2** |
| dumphfdl [21] | GPL-3.0 | HFDL reference and ground-station frequency table |
| Willt125/MIL-STD-188-110 [23] | MIT | A minimal 110C encoder/decoder with an HF channel simulator. Usable for cross-checking our generators |
| fldigi [25] | GPL-3.0 | RTTY, NAVTEX/SITOR-B, MFSK16, PSK references |
| Rivet [24][38] | **GPL-3.0-or-later** (per-file headers; no LICENSE file). Last commit 2014-01-01 | Java decoders for CIS36-50 (50 Bd; 75/200/250/400/500 Hz shifts), CROWD36, XPA (10/20 Bd), XPA2, FSK200/500 (F01), FSK200/1000 (F06), CCIR493-4, GW FSK, plus AT3x04, OFDM and RDFT classes. **No Akula.** Its CIS36-50 code independently confirms the T-600 message structure (row CIS-36-50). It is a **decoder**, so here it is a reference for parameters and structure only; no code copied |
| msdr [7] | GPL-3.0 | Another SDRconnect discovery tool. Shows that labels "with their source" (DSP, schedule, band plan) are a workable UI idea |
| pdn-soundmodem [5][6] | AGPL-3.0 | Has an SDRconnect WebSocket client and probe; the source of the measurements in section 1.2 |
| **Your own repos** | yours | 2G/3G/4G ALE detectors and generators, FSK and SITOR-B receiver, Morse (OOK), PSK31. **Directly reusable** for layer 2 evidence and for synthetic generators |

**Datasets**

| Dataset | Licence | Usefulness |
|---|---|---|
| Scholl 2019 [26] | Licence not verified | 18 synthetic HF modes, but amateur and broadcast, not military |
| Scholl 2025 [27] | Release and licence not stated | The closest prior art: 160 classes including STANAG 4285/4529/4539, 110A/A16/B39, ALE 2G/3G, Link-11 CLEW/SLEW, HFDL, PACTOR 1–4, DSC, SITOR and many CIS modes. 4 kHz IQ, 1 s windows, about 90 % top-1 / 95 % top-3 at high SNR, some classes below 80 % |
| TorchSig [28] | MIT | Generic synthetic modulation families and wideband multi-signal scenes. Good for training the *detector* and generic family classifiers, not for HF military modems |
| RadioML / DeepSig [33] | Reported CC BY-NC-SA 4.0 (not re-verified) | Generic and not HF. Low value here |
| sigidwiki audio [12] | **No reuse licence granted** (its disclaimer says contributors keep their rights). The sdrmm PR calls it CC BY-SA; I found no basis for that | Mostly lossy MP3 audio. **Not redistributable.** Useful only for local smoke tests of spectral and timing features, if you agree |

**Conclusion.** Clean training and evaluation data = **our synthetic generators** (for modes with
public parameters) **plus recordings supplied for the project** (for everything, and as the only source for the modes
in section 3.5).

### 3.5 Recordings and definitions wanted

**Recordings.** Preferably IQ from SDRconnect (its IQ WAV) or SigMF, with the RF frequency and
UTC. Otherwise USB audio, with the dial frequency. Ideally ≥ 2 minutes, from more than one day.

1. **No public spec; recordings are the only source:** CIS-75 FSK, CHN π/2-BPSK 2400, CHN MIL
   8PSK 1627, Turkish Navy FSK 600 Bd, Harris proprietary/ECCM, Serdolik v2 (OFDM traffic),
   CIS-112, CIS-3000.
2. **Only analyst observations exist, so real signals are needed to verify the generators:**
   CIS-36-50 (incl. 50 Bd / 1000 Hz), CIS-50-50, CIS-12 (PSK2 and PSK4), CIS-1200, CIS-20,
   XPA2/XPB, Morse stations, and **Akula II (single-carrier BPSK) / Akula 100 Bd**.
   - **Akula FSK and the 15-carrier DBPSK are now measured on two recordings**
     (section 3.6).
   - Those two files are now **development data**: I derived the preamble words from them. So
     they can't be the held-out test of that same rule.
   - To *test* Akula honestly I need more, independent recordings from other days and
     frequencies, preferably IQ: for example the recordings behind the screenshots of source
     [39], the 9201 kHz KiwiSDR file [40], and the frequency and date of the first file of
     section 3.6.
3. **Confusable pairs where real data decides:** Link-11 CLEW vs 110 App A; Link-11 SLEW vs 110A;
   DSC vs CODAN Selcal; HFDL vs CODAN 3212.

**Definitions** for "16FSK", "FSK 600 Bd families", "CIS MFSK-16" (XPA2 or XPB?) and "Polish FSK
100 Bd" (F11 or POL-ARQ?).

### 3.6 Measurements on two recordings (2026-10-19)

**How I measured:**
- Throwaway numpy scripts outside this repository, written during the research. They are not
  project code.
- **Tone lines:** long FFTs.
- **Baud:** the spectral line of |d(instantaneous frequency)/dt| (FSK), or of the DBPSK
  decisions.
- **PSK order:** carrier lines of x², x⁴ and x⁸.
- **Bits:** integrate-and-dump at the best timing phase, then a search for the *published* Akula
  patterns [35] in both polarities.
- **Only structure is reported.** No payload is printed or kept, and Akula traffic is encrypted
  anyway.

**File 1: an Akula FSK clip.** 6 kHz mono 16-bit audio, 8.28 s, an edited clip padded with
digital silence. Frequency and date unknown.

| Item | Result |
|---|---|
| Content | Two FSK bursts, about 1.60–3.95 s and 4.45–6.65 s |
| Baud | **499.92 Bd** (burst 1) and **500.00 Bd** (burst 2) |
| Tones | **Discrete lines** at **1004.5** and **2004.4 Hz**, so the shift is **999.9 Hz**. FSK with modulation index h = shift / baud = 2 puts lines at the tones, which makes a cheap and strong feature |
| Structure (both bursts) | Dot reversals → a run of 6–7 zeros → **sync 4 × `100101` + 2 × `110001`** (0 errors) → `000000` → **preamble 4 × `111100` + 3 × `100010`** → 142 words of IV/data → **EOM+EOT** (0 errors) |
| Frame length | Both bursts are exactly **966 bits = 1.932 s** from sync to the end of EOT, with 6-bit alignment throughout |
| Polarity | High tone = 1 in USB. The inverted polarity gives at best 5–6 errors on the same 30–36-bit patterns, so polarity is unambiguous |

**File 2: a 15-carrier Akula recording.** 96 kHz
real IF recording from Signals Analyzer (it has an `*SA*` chunk), 5.88 s. Its name gives
7.918 MHz and a 48 kHz span. Frequencies below are IF; the RF mapping (presumably
24 kHz IF ↔ 7.918 MHz) needs your confirmation.

| Item | Result |
|---|---|
| 0.04–2.90 s | **15 carriers exactly 2000.0 Hz apart** (IF 9955…37955 Hz; the spacing is 2000.014 Hz, so the recording's frequency scale is good to ~7 ppm). **Each carrier is BPSK**: the x² line is 45–49 dB above the median, and the x⁴ line sits exactly at twice its frequency. 499.5 Bd per carrier. **All 15 carry the same symbol stream** (pairwise correlation 1.00) |
| 2.92–5.80 s | A **single 2-FSK** on the middle carrier (IF 23955 Hz), 499.5 Bd, shift ≈ 1000 Hz (the IF-median estimate of 994 Hz is biased low) |
| Bit content | The DBPSK stream (after differential detection; inverted convention) and the FSK stream (high = 1) are **bit-identical** over all 690+ bits compared. Each holds a **390-bit frame**: reversals → zeros → sync (0 errors) → `000000` → preamble **2 × `110001` + 2 × `101111` + 3 × `100010`** → … → EOM+EOT (2 errors). That is followed by a **data-only block** of 154 words that ends in EOM+EOT (0 errors in the FSK) and has **no sync**, which matches the "data-only" bursts in [35][36] |
| Baud difference | 499.5 Bd here against 499.92–500.00 in file 1. The frequency scale checks out, so this is not a sound-card clock error, unless SA resampled the file. **Baud tolerance in the catalogue: ±0.3 %** |
| Other signals | Unrelated carriers at IF 20852 Hz (with ±300 Hz sidebands) and 28563/29113 Hz |

**Conclusions.** All come from 2 files and 4 frames, so this is not a statistical test.
1. The published sync and EOM+EOT groups [35] hold on real signals, with 0 errors at good SNR.
   The wrong polarity gives ≥ 5 errors. That suggests a detection threshold of ≤ 3 errors on
   36 bits; a random 36-bit window passes it with probability about 10⁻⁷ per position.
2. **The preamble is not fixed.** The first 4 words vary (`4 × 111100` in one recording,
   `2 × 110001 + 2 × 101111` in the other); the last 3 words (`3 × 100010`) were the same in all
   three frames. EOM+EOT alone must be enough for detection, because data-only blocks exist.
3. **A multi-carrier form exists.** 15 × 500 Bd DBPSK, 2000 Hz apart, about 30 kHz wide,
   repeating the same frame as the FSK that follows. The literature only has "Akula II = a short
   single-carrier BPSK 500 Bd burst" [15]. Whether this is the same thing in multi-carrier form is
   unknown.
   - **A 3 kHz audio classifier cannot see this emission as a whole.** The wideband IQ design
     can, if the tracker **groups a comb of carriers into one emission**: equal spacing, equal
     baud, simultaneous on/off, and identical symbol streams. This is now a requirement for the
     detector and tracker (section 4.2).
4. **These two files are development data now**, not held-out test data (see 3.5).

### 3.7 Frame periods on the wiki's samples (2026-11-09)

**How I measured:** the program's feature meter (`core/src/features.cpp`: the autocorrelation
of the emission's complex baseband at lags of 10 to 650 ms, each lag normalised by the energy
of the two stretches it compares). It was run on 8 s windows of the wiki's audio, one from
1 s in and, for clips over 20 s, one from the middle. A throwaway harness outside this
repository found each sample's band from its averaged spectrum. This is not the full
pipeline; the README's benchmark is. The period reported is the shortest repetition within
6 dB of the strongest that also repeats at twice itself. **These are the samples the
benchmark runs on**, so the benchmark cannot confirm the catalogue periods below
independently.

| Wiki entry (samples) | Period measured | Explanation |
|---|---|---|
| STANAG 4285 (7) | **106.67 ms on all 7**. Two give 105.9 ms, and their symbol rate (2418 Bd) is off by the same 0.75 %: those recordings run slow. Strength 0.49 to 0.96 | The 80-symbol preamble in every 256-symbol frame [11] |
| STANAG 4529 (5) | **213.33 ms on all 5** (one gives 211.7 ms) | The same frame structure at 1200 Bd [11] |
| STANAG 4539 (5) | 119.6 ms on 2. 597.9 ms (5 frames) on 1. 76.7 ms on 1 (unexplained). None on 1, whose correlation peaks at 239.2 ms (2 frames), too weakly to pass | 256 data + 31 probe symbols [12]. The probes are not the same in every frame, so the whole repeats only after several |
| MIL-STD-188-110 serial (20) | **66.67 ms on 14**: all 9 whose names give 75 to 1200 bps, and 5 whose names give no rate. **200 ms on 5**: the 2400 and 4800 bps samples, one 150 bps sample with the long interleaver (its 4.8 s preamble fills the window measured), and 2 whose names give no rate. None on 1 (a 1 s clip) | The scrambler restarts every 160 symbols [42], so the known probe symbols repeat after LCM(160, 40) = 160 symbols (66.67 ms) with 20 data / 20 probe frames, and after LCM(160, 48) = 480 symbols (200 ms) with 32/16 frames [11]. The preamble is built of 200 ms segments [11]. This derivation is mine; the measurements agree with it |
| 110A-MOD, Israeli Navy hybrid modem (7) | 66.67 ms on 5, none on 2 | 20/20 frames [11], as above |
| CIS-1200 (3) | 426 ms in 1 window of 1 sample; 12.5 ms (15 symbols) or 25 ms on the others | A 512-bit block [12]. The 12.5 ms pattern is unexplained (an idle state?) |
| Link-11 SLEW (the 2 samples named so) | 413.3 ms on one; there, the 133.33 ms that [42] shows (LCM(160, 64) = 320 symbols) is present but 9 times weaker. None on the other | — |
| 3G ALE (5), 4G ALE (2), HFDL (5) | Bursts: mostly none. HFDL 25 to 300 ms, different on each sample | — |
| Others (39-tone, CIS-12 idle, CIS-3000) | 22.5 ms (the 39-tone symbol, during a preamble or an idle state), 25 ms (CIS-12 idle, 3 symbols at 120 Bd), 85.3 ms (CIS-3000) | Idle and preamble patterns. Not catalogued |

**The symbol rate was being measured wrong on framed signals.** A frame puts a line at every
multiple of 1/frame in the envelope spectrum that the symbol rate is read from. Before the
meter took that into account, it read 4 of the 7 STANAG 4285 samples as 690 to 1474 Bd, the
4529 samples as 342 to 811 Bd, and most 110A samples as 795 or 1200 Bd. Measured against the
frame's six lines on either side, a symbol rate's line stood 13 to 22 dB above them, while
the frame's own lines stood at most 12.8 dB above theirs. The exception is 110A-MOD: its lines
every 75 Hz stand up to 22 dB above the others, so its rate is still misread. The catalogue
matcher therefore lets an agreeing frame override a disagreeing rate.

The frame's lines also showed up as "tones" 15 Hz (or a multiple of 4.69 Hz) apart, which made
110A and 4529 samples look multi-carrier. Close lines (under 60 Hz apart) spaced at a small
multiple of 1/frame are now set aside as the frame's. Where they still are not (4.69 Hz is
finer than the averaged spectrum resolves), an agreeing frame lists the mode all the same.

**Through the whole pipeline** (the README's benchmark of 2026-11-09, each sample in two noise
runs), a right mode came first in the consistent list, with its frame agreeing, for 41 of these
49 samples: STANAG 4285 7 of 7, 4529 5 of 5, 4539 1 of 5, MIL-STD-188-110 serial 18 of 20, 4415
2 of 2, 110A-MOD 7 of 7, CIS BPSK 1200 1 of 3. On 2026-11-07, before the frame, for none.

---

## 4. Proposed architecture and stack

### 4.1 Data flow

```
 SDRconnect (GUI or headless, ≥1.0.9) ──WS 127.0.0.1:5454──┐   JSON control: LO, rate, LNA, antenna, device
                                                            │   Binary IQ: type 2 (input A), type 5 (input B)
 File source (SigMF, SDRconnect/SDRuno IQ WAV) ─────────────┤
                                                            ▼
 Acquisition: one reader thread per input → lock-free ring buffer (≥ 10 s) with a sample counter
   → UTC anchor; gap / retune / overload flags                                    ──► SigMF recorder
                                                            ▼                          (window, or per-
 Wideband analysis: windowed FFT (overlap-save), Welch PSD frames                       emission pre-roll)
   → noise floor (per-bin order statistics over time and frequency) → OS-CFAR detections
                                                            ▼
 Emission tracker: time-frequency blobs → tracks (start/end UTC, f_c and drift, OBW, SNR,
   duty cycle, burst list) ; splits and merges ; per-input
                                                            ▼
 Per-emission DDC: FFT-bin extraction from the same overlap-save FFTs → complex baseband at
   about 4 × OBW (e.g. 12 kS/s for a 3 kHz channel)
                                                            ▼
 Feature extractors (thread pool) → feature vector + "evidence" items
                                                            ▼
 Classifier
   L1 parametric rules vs catalogue (YAML) ─┐
   L2 sync/preamble correlators ────────────┼─► calibrated fusion → top-N + confidence
   L3 ML (ONNX: GBM on features; small CNN) ┘      + "unknown / similar to X" (open set)
   Frequency-DB match shown SEPARATELY (optional prior, off by default)
                                                            ▼
 SQLite ──► local web GUI (waterfall + spectrum + labelled boxes, emissions table, results
            viewer, filters) ──► CSV / JSON export
```

### 4.2 Feature extraction, per emission

| Measurement | Method |
|---|---|
| Occupied bandwidth and spectral shape | 99 % power and −26 dB widths, flatness, symmetry, edge sharpness, carrier lines |
| FSK / MFSK | Instantaneous-frequency histogram (tone count, shift); a long-FFT tone comb for spacing; per-tone dwell statistics. XPA2 at 15 Hz / 7.5 Bd needs FFT resolution ≤ 2 Hz, so it is done after the DDC, not in the wideband FFT |
| Symbol rate | Cyclostationary lines: \|x\|² (and \|x\|² after a matched-filter guess) for PSK/QAM; the derivative of instantaneous frequency for FSK; a refined search on a narrow grid [32] |
| PSK order / QAM | Carrier lines of x², x⁴ and x⁸ (BPSK/QPSK/8PSK; π/2-BPSK signature); higher-order cumulants C40/C42/C63 [32]; a blind constellation estimate after timing recovery. **No equalisation, no symbol decisions kept** |
| OFDM / multitone | Tone count and spacing (comb fit); per-tone symbol rate; cyclic-prefix autocorrelation (T_u, T_cp); pilot offsets (+3300 Hz CIS, 605 Hz Link-11, 393 Hz App B) |
| Frame structure | Autocorrelation of the baseband or of x^M at the known periods (106.67, 213.33, 119.6, 426.67 ms …) — the sdrmm method [22]. **Done** for the baseband (any period of 10 to 650 ms; section 3.7) |
| Bursts and timing | Envelope segmentation; burst length and repetition (SITOR-A 450 ms, PACTOR 1.25/3.75 s, HFDL slots, ALE bursts) |
| Morse | Keying-envelope histogram (dot/dash/gap ratios, WPM). **No character decoding** |
| Multi-carrier grouping | The tracker groups carriers that share spacing, baud and on/off times into **one** emission, and confirms it by cross-correlating their symbol streams. Required by the measured 15-carrier Akula DBPSK (section 3.6) |
| FSK tone lines | Discrete lines at the tones when h = shift/baud is an integer (Akula: h = 2, measured) |
| Polarity | USB vs LSB hypotheses are evaluated for every tone plan |

### 4.3 Classifier layers

- **L1 — parametric rules.** The catalogue is YAML. Each parameter is a range with its
  tolerance, its provenance tag and its source. Each rule returns a likelihood, not a yes/no.
- **L2 — sync/preamble correlators, as evidence only.** Correlation with *known public*
  sequences only:
  - 4285, 4529, 110A and 4539 preambles;
  - ALE 2G, 3G BW0/BW1, and Deep/Fast WALE (reused from the author's plugin repository);
  - DSC/Selcal dot pattern and phasing; T-600 SOM and EOM [13][37][38];
  - Akula sync group and EOM+EOT [35].
  - **Code-structure statistics.** These are tests on the demodulated bits that never produce
    characters:
    - the share of 7-bit words with 4:3 constant weight (T-600; also SITOR, so it only counts
      together with baud and shift);
    - 6-bit word framing (Akula).
  - Every bit-pattern test runs in **both polarities**, because USB/LSB and mark/space conventions
    differ between sources.
  - No payload bits are output or stored. Only correlation scores and statistics are kept.
- **L3 — ML.**
  - Stage 1: gradient boosting on the L1 features. It is fast and explainable.
  - Stage 2: a small CNN on short spectrograms or baseband IQ windows.
  - Both are trained on synthetic data plus your labelled recordings.
  - Open-set "unknown": a distance to class prototypes, plus a reject threshold calibrated on
    held-out unknowns.
- **Fusion.** Calibrated per-layer scores (temperature scaling, checked with reliability
  diagrams) are combined into top-N with confidence. The per-layer breakdown is stored and shown,
  so "why this label" is visible.
- **Frequency-database matches** (station names) are displayed next to each emission but kept
  **out of the signal-evidence confidence by default**. Using them as a prior is a setting, and
  both numbers are shown.

### 4.4 Stack (recommendation)

| Part | Choice | Why | Licence |
|---|---|---|---|
| DSP core and runtime | **C++17, CMake** (MSVC 2022 on Windows; GCC/Clang in CI) | Real-time headroom for 6–8 MS/s and many channels. **Direct reuse of your existing C++17 DSP** (ALE correlators, FSK/SITOR, OOK, PSK, generators) and of your Windows CI and NSIS installer pattern | — |
| WebSocket client | IXWebSocket (or Boost.Beast) | Small, works on Windows | BSD-3 (BSL-1.0) |
| FFT | pocketfft (header-only) | No GPL; FFTW is GPL | BSD-3 |
| Database | SQLite | Single file, offline | Public domain |
| Configuration and catalogue | TOML (toml++) for config; YAML (rapidyaml) for the catalogue | As you asked | MIT |
| ML inference | ONNX Runtime (C++) | The runtime needs no Python | MIT |
| Training and benchmarks (offline only) | Python 3.12: numpy/scipy, LightGBM, PyTorch → ONNX | Fast iteration on models | BSD / MIT |
| GUI | **Local web GUI** served by the core on 127.0.0.1 (HTML/JS, WebGL waterfall, **all assets bundled, no CDN**) | Avoids Qt build and licence weight; works fully offline; same model as SDRconnect's own web client. A Qt 6 desktop GUI is possible if you prefer it | — |
| Recording format | SigMF (our own writer for `.sigmf-meta` / `-data`) | Avoids the LGPL dependency | Spec: CC BY-SA 4.0 [29] |

**Alternative if you prefer it:** Python-first, with numpy/numba for the DSP and C++ only where a
benchmark demands it. It iterates faster, has less real-time headroom, and reuses your C++ code
less directly.

### 4.5 Database (sketch)

| Table | Contents |
|---|---|
| `inputs` | Inputs and their settings |
| `emissions` | id, input, f_c, OBW, t_start/t_end UTC, SNR, duty cycle, drift, flags |
| `observations` | Per-window measurements |
| `features` | JSON, versioned |
| `classifications` | emission, rank, class, confidence, per-layer scores, catalogue version, model version |
| `recordings` | SigMF path and metadata |
| `stations` | The imported frequency DB |
| `station_matches` | Links between emissions and stations |
| `labels` | Your manual labels, which become training data |

All times are UTC (ISO 8601 + integer µs).

**Frequency DB import/export (CSV):** `freq_khz, tolerance_hz, mode, station, callsign,
country, schedule_utc, source, notes`. A match is any entry within tolerance + OBW/2.

### 4.6 Testing and benchmarks

- **Generators** for every catalogue entry that has public parameters. Each can vary baud, shift,
  tone plan, SNR, CFO, drift and clock ppm. Channel effects:
  - HF fading: the Watterson two-path model with ITU-R F.1487 channel sets [30][31];
  - interference: carriers, adjacent signals, impulsive noise;
  - USB/LSB polarity.
- **Loaders** for your WAV/SigMF recordings, with label files.
- **Leakage control:** held-out splits by generator seed *and* by recording session/day.
- **Metrics:**
  - per-class precision/recall/F1 and confusion matrix;
  - accuracy vs SNR per class;
  - top-N accuracy and calibration (ECE);
  - open-set AUROC;
  - false alarms on noise only and on unlisted signals;
  - CPU per channel, and the maximum number of parallel channels in real time on the user's PC.
- **"Working" gate.** A class is labelled:
  - **synthetic-verified** after it passes on held-out synthetic data;
  - **verified** only after it passes on held-out *real* recordings.
  - I propose a gate of P ≥ 0.90 and R ≥ 0.90 at SNR ≥ 10 dB in 3 kHz; you set the threshold.
  - A class without both stays "experimental" in the UI.

### 4.7 Milestones after approval

| # | Milestone |
|---|---|
| 1 | **SDRconnect probe** (section 1.3), acquisition, ring buffer, SigMF record/playback. Report the measured RSPduo facts |
| 2 | Wideband detector and tracker, SQLite, minimal web GUI (waterfall + boxes + table) |
| 3 | Feature extractors, catalogue v0 (documented classes), L1 rules, generators, benchmark harness |
| 4 | L2 correlators for the public preambles and sync sequences |
| 5 | L3 ML and open-set; calibration |
| 6 | Evaluation on real recordings; per-class reports; per-module READMEs (parameters and limitations); Windows installer |

### 4.8 Legal note

Passive reception and the recording of non-broadcast traffic are regulated differently from
country to country. The tool decodes no content, but IQ recordings still contain the signals.
Check your local rules before you keep or share recordings.

---

## 5. Decisions that were needed

1. **Where the tool lives:** a new repository, or a new folder in one of the author's
   SDRuno plugin repositories?
2. **Stack:** the C++17 core + offline Python training + local web GUI (recommended), or
   Python-first?
3. **Definitions:** CIS-75 FSK, CIS MFSK-16 (XPA2 or XPB?), 16FSK, FSK 600 Bd families, Polish
   FSK 100 Bd (F11 or POL-ARQ?), the Harris mode, CHN π/2-BPSK 2400 and CHN MIL 8PSK 1627, Turkish
   Navy FSK 600 (sources, if any).
4. **Recordings** for the project (section 3.5).
5. **The setup:** SDRconnect version, Windows version, CPU and RAM, and which antennas go to
   which RSPduo port.
6. **Frequency DB source and format**, and its licence. *Answered 2026-11-10: the free lists
   that WebSDR and KiwiSDR sites use for their markers. The program reads them from a folder
   (text, CSV, KiwiSDR `dx.json`, OpenWebRX bookmarks) and ships none, since none grants a
   right to redistribute; only nine ITU frequencies are built in (`core/README.md`, Known
   frequencies).*

---

## Sources

How each source was used: **read** = read in full in this session; **summary** = only a search or
fetch summary was seen; **memory** = not re-read in this session.

| # | Source | Used | Licence / notes |
|---|---|---|---|
| 1 | SDRplay, *SDRconnect WebSocket API 1.0.3*. https://api.sdrplay.com/docs/SDRconnect_WebSocket_API.pdf | read | © SDRplay |
| 2 | SDRplay, *SDRconnect Release Notes* V1.0.10. https://sdrplay.com/wp-content/uploads/2026/08/SDRconnect_Release_Notes.pdf (V1.0.9: https://api.sdrplay.com/docs/SDRconnect_Release_Notes.pdf) | read | © SDRplay |
| 3 | SDRplay, WebSocket API page and C# examples (`SDRconnectWebSocketAPI`: Client, MinimalDuoExample, RawIqWriter, SDRconnectCtl). https://sdrplay.com/websocket-api, https://sdrplay.com/download/websocket-api-c-examples/ | read | No licence file in the zip |
| 4 | SDRplay, SDRconnect download page. https://www.sdrplay.com/sdrconnect/ | read | — |
| 5 | M0LTE, SDRconnect probe results, pdn-soundmodem issue 701. https://github.com/packet-net/pdn-soundmodem/issues/701 | summary | Project AGPL-3.0 |
| 6 | pdn-soundmodem PR 702 (sdrconnect: device). https://github.com/packet-net/pdn-soundmodem/pull/702 | summary | AGPL-3.0 |
| 7 | msdr. https://github.com/gdamdam/msdr | summary | GPL-3.0 |
| 8 | SDRconnect Preview 2 announcement (secondary report). https://machamradio.com/blog/2023/11/28/sdrconnect-updated-november-27th-2023/ | summary | — |
| 9 | SDRplay, *RSPduo datasheet V0.6* (mirror). https://sdrplay.nl/wp-content/uploads/RSPduoDatasheetV0.6.pdf | read | © SDRplay |
| 10 | SDRplay, *SDRplay API Specification v3.15*. https://api.sdrplay.com/docs/SDRplay_API_Specification_v3.15.pdf | read (RSPduo sections) | © SDRplay/Mirics |
| 11 | Wavecom, decoder online help, mode pages: CIS-12, CIS-36, CIS-36-50, CIS-50-50, STANAG-4285/4415/4529, MIL-188-110A, 110A-MOD, 110B App C/4539, 141B App C, LINK-11 CLEW/SLEW, CODAN Selcal/3212/Chirp/9001, GMDSS/DSC-HF, SITOR-ARQ/FEC, PACTOR I/II/III, POL-ARQ, Chinese 4+4, MFSK-8/16, CW-Morse. https://www.wavecom.ch/content/ext/DecoderOnlineHelp/ | read | © Wavecom. Used as a reference only |
| 12 | Signal Identification Wiki (raw pages for every mode cited). https://www.sigidwiki.com | read | No reuse licence granted (General disclaimer) |
| 13 | Priyom.org, *T-600*. https://priyom.org/military-stations/russia/t-600 | summary | — |
| 14 | Priyom.org, *Serdolik 2018*. https://priyom.org/diplomatic-stations/russia/serdolik-2018 | summary | — |
| 15 | i56578-swl, *FSK 500Bd/1000, CIS Navy Akula*. http://i56578-swl.blogspot.com/2015/07/cis-navy-shark-akula.html | summary | — |
| 16 | i56578-swl, *Serdolik OFDM 35-tone DQPSK*. http://i56578-swl.blogspot.com/2015/08/serdolik-ofdm-341-tones.html | summary | — |
| 17 | i56578-swl, CIS-1200 BPSK/SDPSK "Makhovik" posts (2021-03, 2024-07), as linked from [12] | not read | — |
| 18 | radioscanner.ru forum, T-230-1A thread. https://www.radioscanner.ru/forum/topic34543.html | summary (text garbled by encoding) | — |
| 19 | Harris RF-5800H-MP brochure/specifications (third-party mirrors). http://www.railce.com/cw/casc/harris/rf-5800h-mp.pdf | summary | © Harris/L3Harris |
| 20 | The author's repository `SDR_plugin_NAVTEX/docs/ALE.md` (2G/3G/4G ALE implemented from MIL-STD-188-141B/D) | read | The author's |
| 21 | dumphfdl. https://github.com/szpajder/dumphfdl | read (README) | GPL-3.0 |
| 22 | sdrmm PR 139, *STANAG modem identifier*. https://github.com/Newspicel/sdrmm/pull/139 | summary | AGPL-3.0 |
| 23 | Willt125/MIL-STD-188-110. https://github.com/Willt125/MIL-STD-188-110 | summary | MIT |
| 24 | Rivet. https://github.com/IanWraith/Rivet | read (README.txt, source list) | GPL-3.0-or-later (file headers) |
| 25 | fldigi. https://github.com/w1hkj/fldigi | — | GPL-3.0 |
| 26 | S. Scholl, *Classification of Radio Signals and HF Transmission Modes with Deep Learning*, arXiv:1906.04459 (2019) | summary | — |
| 27 | S. Scholl, *Large-Scale Classification of Shortwave Communication Signals with Machine Learning*, arXiv:2504.05455 (2025) | summary | Page CC BY-NC-ND 4.0 |
| 28 | TorchSig. https://github.com/TorchDSP/torchsig | read (LICENSE) | MIT |
| 29 | SigMF specification. https://github.com/sigmf/SigMF | read (LICENSE) | CC BY-SA 4.0. sigmf-python is LGPL-3.0+ |
| 30 | ITU-R F.1487 (testing HF modems with channel simulators); ITU-R M.493 (DSC); ITU-R M.476 / M.625 (SITOR); ITU-R SM.1600 (technical identification of digital signals) | memory | ITU (free download) |
| 31 | C. C. Watterson et al., "Experimental confirmation of an HF channel model", *IEEE Trans. Commun. Technol.*, 1970 | memory | — |
| 32 | W. A. Gardner, "Exploitation of spectral redundancy in cyclostationary signals", *IEEE SP Mag.*, 1991. O. A. Dobre et al., "Survey of automatic modulation classification techniques", *IET Commun.*, 2007. A. Swami, B. Sadler, "Hierarchical digital modulation classification using cumulants", *IEEE Trans. Commun.*, 2000 | memory | — |
| 33 | DeepSig RadioML datasets | memory | Reported CC BY-NC-SA 4.0; verify |
| 34 | SDRplay video, *Using the RSPduo with SDRconnect*. https://www.youtube.com/watch?v=Up6WpT7pbTU | not watched | — |
| 35 | i56578-swl, *Akula always reserves surprises* (4 May 2024). http://i56578-swl.blogspot.com/2024/05/akula-always-reserves-surprises.html | summary | — |
| 36 | i56578-swl, *CIS-Navy & Akula transmission* (FSK 50Bd/1000 before Akula, 9202 kHz CF), text supplied by you on 2026-10-19 | read | — |
| 37 | i56578-swl, *CIS Navy 50Bd/200 FSK (T600, BEE-36, CIS-36)* (Oct 2016). http://i56578-swl.blogspot.com/2016/10/cis-navy-50bd200-fsk-t600-bee-36-cis-36.html | summary | — |
| 38 | Rivet source `src/org/e2k/CIS3650.java` (commit 41f81b2, 2014-01-01). https://github.com/IanWraith/Rivet | read | GPL-3.0-or-later. Reference only |
| 39 | Signals Analyzer (SA) screenshots supplied for the project: FSK 100 Bd/1000, BPSK 500 Bd + FSK 500 Bd/1000, FSK 500 Bd, and the 6-bit bitmaps with EOM+EOT | read | Not published |
| 40 | i56578 recording `jf0fumkiwi.ddns.net_2025-04-16T12_24_11Z_9201.00_usb.wav` (2.7 MB, KiwiSDR USB audio). https://disk.yandex.com/d/E29PupqpJg3UTQ | listed, **not downloaded** (storage host times out from this container) | Licence unknown; author's file |
| 41 | Two recordings supplied for the project: an Akula FSK clip (6 kHz audio, 8.28 s) and a 15-carrier Akula recording (96 kHz real IF, 5.88 s) | analysed (section 3.6) | Not published. Development data |
| 42 | i56578-swl, *STANAG 5511 (Link-11 SLEW) scrambler* (13 May 2016), quoting MIL-STD-188-110A: "After 160 transmit symbols, the shift register is reset to BAD (hex) prior to the eight shifts"; 64-symbol SLEW frames, 133.33 ms correlation peaks. https://i56578-swl.blogspot.com/2016/05/stanag-5511-link-11-slew-scrambler.html | summary (fetched 2026-11-09) | — |
