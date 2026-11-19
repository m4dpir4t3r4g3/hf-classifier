# core: the connection to SDRconnect, detection and decoding

Portable C++17. The connection needs nothing beyond the standard library
and the platform's sockets (Winsock 2 on Windows); the signal processing
uses KISS FFT and ft8_lib (`third_party/`) and the decoders copied from the
SDRuno plugin repositories (`decoders/`).

| File | What it is |
|---|---|
| `net` | Blocking TCP client and listener |
| `codec` | SHA-1 and Base64, for the WebSocket handshake |
| `websocket` | WebSocket (RFC 6455) client and server side: handshake, framing, fragments, ping/pong, close. No extensions, no TLS |
| `json` | Reader for flat JSON objects (SDRconnect's messages), writer for any JSON (SigMF metadata, reports) |
| `sdrconnect` | The API's names: event types, properties, binary payload types, message encoding |
| `client` | `sdrconnect::Client`: connection, property cache, `query`/`setAndConfirm`, IQ/audio/spectrum of the primary and secondary device |
| `iq_clock` | Sample numbering, UTC estimate and gap detection for an IQ stream without timestamps |
| `sigmf` | SigMF writer (ci16_le, captures, annotations) and reader (ci16_le, cf32_le, ci8, cu8) |
| `dsp` | FFT (KISS FFT, sizes of factors 2, 3, 5) and an oscillator |
| `channelizer` | One overlap-save FFT for the wideband spectrum and any number of 12 kHz channels |
| `detector` | Noise floor, CFAR, segments, and the emission tracker (start, end, band, SNR, duty, drift) |
| `ftx_decoder` | FT8 / FT4 decoding of one UTC slot with ft8_lib |
| `decoder_host` | A channel per emission, turned into USB audio, fed to the decoders that fit its bandwidth; quality gates; FT8/FT4 sub-bands |
| `pipeline` | Channelizer + detector + decoder host, sample numbering to UTC, retunes, a reduced waterfall |
| `store` | The SQLite log: sessions, emissions, decodes; queries, CSV and JSON export |
| `web_server` | HTTP/1.1 and WebSocket server for the local UI (127.0.0.1, Host and Origin checked) |

## From IQ to emissions and decodes

```
IQ (int16, any SDRconnect rate) -> Channelizer: FFT of N = rate / 15.6 Hz (31 Hz above 2.6 MS/s), 50 % overlap
   |-> Hann power spectrum per hop -> Detector: frames of 0.128 s -> noise floor -> CFAR (+7 dB), weak wide regions
   |                                  -> segments -> tracker (overlapping, or starting together and close: one emission)
   |                                  -> emissions (confirmed after 3 frames, ended after 3 s unseen)
   `-> 12 kHz channels -> DecoderHost: one per emission -> USB audio -> candidate decoders -> quality gate -> decodes
                                       one per FT8/FT4 sub-band -> UTC slots -> ft8_lib (worker thread) -> decodes
```

Channels are exact: every SDRconnect rate gives integer K = N * 12000 / rate,
so a channel comes out at exactly 12000 S/s, centred to within a millihertz,
with continuous phase across FFT blocks (tested at 62.5 kS/s to 10 MS/s).

The channelizer keeps the FFT spectra of the last 0.75 s (at most 64 MB:
0.75 s up to 2 MS/s, about 0.3 s at 10 MS/s), and a channel added now can be
run over them as if it had existed then (`replay`). A session starts once
the detector has confirmed its emission, three 128 ms frames after it
began; its decoders are given the 0.75 s before as well, so the preamble of
a 3G or 4G ALE burst (its first 120 to 240 ms, without which the burst
cannot be found) and the first characters of a transmission are not lost.
Without it, isolated 3G and 4G bursts were never decoded. The feature meter
still starts at the emission's start.

The detector groups emissions that start within 0.4 s of each other, less
than 1.1 kHz apart and within 10 dB in SNR: the tones of an FSK or MFSK
signal and the carriers of a multi-carrier modem become one emission (not
in the FT8/FT4 sub-bands, where every station starts with the slot). It
measures the occupied bandwidth as the band holding 99 % of the emission's
power (summed over its life; ITU-R SM.328), which the decoder host uses: the
detected band of a strong signal includes window skirts and key clicks.

A modem only a few dB above the noise (a 2400 Bd serial-tone modem, say)
has most of its bins under the 7 dB threshold in any one frame, and its
fragments came and went as separate emissions (in the test, five
overlapping ones adding up to 33 s for a 10 s modem). So the detector also
takes the running median of each frame over 180 Hz: a region at least
600 Hz wide where it stands 3 dB above the floor is one segment, taking in
the threshold's segments inside it. A median, unlike a mean, stays low
between separate narrow signals: six carriers 150 Hz apart stay six. Not in
the FT8/FT4 sub-bands, and not where a segment inside peaks 15 dB above the
floor: a strong signal's segments are right as they are, and the keying
sidebands of a strong 850 Hz FSK signal would otherwise join one of its
tones and pull its centre.

The decoder host's candidates, by occupied bandwidth B:

| B | Decoders tried | Believed when |
|---|---|---|
| <= 700 Hz | CW | over the last 32 characters >= 8, <= 25 % undecodable, mean confidence >= 0.6, >= 5 different, <= 50 % of E I S H T 5, no character more than 60 % (as for every character decoder); and plausible text (below). Up to 700 Hz because the measured band of a strong, hard-keyed tone (key clicks, a receiver's AGC) is often that wide |
| <= 250 Hz | PSK31, PSK63 (PSK125 from 100 Hz), and QPSK at the same rates | >= 8 characters while its squelch is open, <= 10 % unprintable, decoder quality >= 0.5 (QPSK: squelch and quality 0.25, see below), and plausible text |
| 250 .. 800 Hz | PSK250, QPSK250 | as PSK above |
| 500 .. 1250 Hz | PSK500, QPSK500 | as PSK above |
| a measured single-carrier PSK signal at 31.25, 62.5, 125, 250 or 500 Bd (within 3 %) | BPSK and QPSK at that rate, whatever the bandwidth (the measured band of a PSK signal varies a lot with its level); added after 3 s of audio | as PSK above |
| 200 .. 700 Hz | SITOR-B / NAVTEX, RTTY 45.45 Bd / 170 Hz | SITOR-B: >= 10 characters, <= 15 % unrecoverable after FEC, >= 4 different. RTTY: the same with <= 20 %, and plausible text |
| 200 .. 700 Hz | DSC (GMDSS digital selective calling, 100 Bd / 170 Hz) | a call whose phasing was found, with every symbol valid in one of its two copies and the error-check character right: believed at once |
| 350 .. 900 Hz | RTTY 50 Bd / 450 Hz | as RTTY above |
| any, on a frequency a list names for it | SITOR-B / NAVTEX, DSC, CW, PSK and QPSK, and RTTY at the rate and shift the list gives ("RTTY 50/450", "50 Bd 450 Hz"); see *Known frequencies* | as for each above |
| a measured 2-FSK signal, shift 60 .. 1100 Hz | RTTY at the measured shift (snapped to 85, 170, 200, 400, 425, 450, 500, 850 or 1000 Hz within 7 %) and rate (snapped to a common one within 3 %), and at 45.45, 50, 75 and 100 Bd; added after 3 s of audio, named e.g. "RTTY 75/850" | as RTTY above |
| 1300 .. 3600 Hz | ALE 2G, ALE 3G/4G | any frame passing Golay (2G) or CRC (3G/4G); a 3G/4G burst whose preamble was found but whose data failed its CRC is still reported, as "Deep WALE burst received intact, data encrypted (linking protection) or not to the standard" when every received coded bit lies on the code word decoded, else as "BW0 burst, data not decoded (too weak or damaged)" (see Limitations) |
| 1100 .. 2200 Hz | Akula (recognised, not decoded) | a burst of 500 Bd on a 1000 Hz shift ending in the published end group (at most 2 of its 30 bits wrong) with the published sync group (at most 3 of 36 wrong) a whole number of 6-bit words before it. Reported as "burst with Akula's sync and end groups, 155 six-bit words between them; content encrypted, not shown" |
| a measured 2-FSK signal | "4:3 code" (a code recognised, nothing decoded): at 50 and 100 Bd, and at a measured 36, 75 or 150 Bd; not 100 Bd on 170 Hz (SITOR) | 45 of 60 consecutive 7-bit words with four bits of one kind and three of the other (chance: 27 %, about 40 % at the best alignment). Reported as "7-bit words of constant weight 4:3 (83 % of 60 in a row): the kind of code CIS-36-50 and CIS-50-50 (T-600), SITOR and several ARQ systems use; content not read". The rate and shift (the consistent modes) say which of them it can be |
| FT8/FT4 sub-band | FT8, FT4 | any message passing LDPC and CRC; at most 50 per slot |

**Plausible text** (CW, PSK, RTTY, which have no error detection of their
own): over the last 16 whole words, at least 6 of them, at least 30 % are
callsigns, words of a vocabulary of radio traffic, English, German and
weather words, or signal reports; with two different strong ones among
them (callsigns with two or three letters after the digit, or repeated;
"CQ"; vocabulary words of three letters or more), or a callsign and "DE";
and fewer than a third of the words garbage (letters and digits mixed in
a word that is no callsign, or more than 12 characters). Or most words are
five-character groups (coded traffic). The callsign and garbage rules came
from sigidwiki's THROB sample, which the Morse decoder made into
"NO0EO9 ET1N H5HEYN ... OR1N" (`tools/sigidwiki_bench.py`).
Without this rule, these decoders printed garbage on signals of other
modes: 40 to 80 false "CW", "PSK63" and "RTTY" decodes per test scene of
21 random-data FSK, MFSK, BPSK and keyed signals; with it, none in 24 such
scenes. The price: CW, PSK or RTTY text that matches none of these (a
language other than English or German, without callsigns) is not decoded.

**QPSK** (fldigi's QPSK31 to QPSK500, decoded with a Viterbi decoder in
either sense of rotation): the decoder's quality is the consistency of the
fourth power of the phase changes, which reads 0.2-0.4 on a weak signal its
convolutional code still decodes cleanly (where BPSK of the same rate reads
about 0.8). Its squelch opens, and its gate passes, at 0.25 instead; the
plausible-text rule keeps out what it prints on other signals (nothing on
the test scenes of other modes' signals).

The first candidate to pass wins the emission; the others are dropped. Its
text is published in lines (or 64-character chunks, or after 3 s, cut at a
space) as `DecodeEvent`s; the first marks the emission's identification.

### Features of every emission

Each emission given a session (every one up to 2.6 kHz wide, decoded or
not; those no decoder takes only while three quarters of the sessions are
free) is measured on its first 8 s of audio by `FeatureMeter`
(`features.hpp`):

- **envelope**: its variation (standard deviation / mean), the same while
  the signal is on (gaps between frames do not count), and the fraction of
  time it is off;
- **FSK and MFSK tones**: the peaks of the histogram of the frequency
  discriminator; if that shows fewer than two, of the discriminator
  smoothed over a third of a symbol of a keying rate found (so that
  MFSK16's tones, 15.6 Hz apart, separate; smoothing first put 200 Bd FSK
  on a false third level between its tones). They count when the envelope
  is constant while on, the discriminator spends 60 % of its time on them
  and no more than 80 % on one (a PSK signal sits on its carrier), and no
  fast single carrier explains the band (below). Two levels that hold the
  signal tightly (over half its weight within a tenth of their gap, a deep
  dip between them) count with a varying envelope too (up to 0.35 while
  on: a receiver's filter, selective fading) and against a line that looks
  like a single carrier's rate, unless the edge clock says that line is the
  clock itself (a PSK signal idling on a pattern) and not a multiple of it
  (FSK keyed through a filter);
- **FSK symbol rate** (two levels): edges from a trigger with hysteresis
  (a fifth of the shift either side of the middle), each timed where the
  discriminator crossed the middle. The clock comes from the intervals
  between edges of the same direction: a threshold off the middle (unequal
  tones, a filter's slope) lengthens every mark and shortens every space by
  the same amount, which those intervals do not see. The unit is the
  largest at which the intervals are coherent (0.5 or more, and within 0.6
  of the best: shaped keying moves the edges of short elements, so that a
  third or a sixth of the unit often fits better); the bit is half the
  shortest interval that is common (a tenth of them or more), which is a
  mark and a space of one bit each, whatever fraction of a bit the unit is
  (RTTY's 1.5-bit stop bits put its edges on a half-bit grid). The signal
  counts as off only after 5 ms below half its level: tones that are
  cross-faded, as sound-card programs send RTTY, dip at every transition.
  More than two levels: the intervals between consecutive crossings,
  doubled when none is one unit long;
- **frame**: the period at which the signal repeats itself, from the
  autocorrelation of its complex baseband at lags of 10 to 650 ms (by FFT;
  each lag normalised by the energy of the two stretches it compares). A
  modem that sends the same known symbols in every frame repeats at its
  frame: STANAG 4285's 80-symbol preamble every 256 symbols gives 106.67 ms
  at a strength |R(T)|/R(0) of about 0.31. Taken only when the peak stands
  8 times above the median of all the lags searched, 4 times above that of
  the lags around it (a carrier, two FSK tones' beat and MFSK's symbols
  correlate broadly, not at one lag), above max(0.05, 8 / sqrt(bandwidth x
  duration)) (random data stays near 1 / sqrt of that), and repeats at
  twice the period (a single echo, such as a cyclic prefix, does not). The
  shortest such period within 6 dB of the strongest is the frame;
- **single carrier (PSK)**: a line in the spectrum of the power envelope
  between 0.3 and 1.25 times the occupied band, at least 15 dB above its
  surroundings (noise alone reaches about 10 dB in that search): the
  symbol rate explains the band (PSK31: 31 Bd in about 60 Hz; serial modems:
  2400 Bd in 2.4-3 kHz). A frame puts a line at every multiple of 1/frame
  in that spectrum, so on a framed signal a line must also stand 13 dB
  above the frame's six lines on either side: without that, the wiki's
  STANAG 4285 and 4529 samples read as 342 to 1474 Bd. The frame's lines
  are not tones either (close lines, under 60 Hz apart, spaced at a small
  multiple of 1/frame). A wide framed signal with a varying envelope and no
  carriers is PSK even without a rate line (a narrow roll-off leaves almost
  none: STANAG 4529's 1200 Bd in 1240 Hz), and so is a framed one whose
  rate fills most of its band, in bursts or fades;
- **multi-carrier**: 6 or more evenly spaced lines with dips between them,
  or, when each subcarrier's keying makes its hump overlap the next, a
  periodicity of the band's spectrum (its log, detrended, autocorrelated;
  the fundamental lag, confirmed at twice the lag), only for a band at least
  6 dB above the noise around it; the per-tone rate from a line below 0.3
  times the band;
- **modulation**, only when the measurements agree: OOK (one tone, keyed),
  carrier, FSK, MFSK (3 to 64 evenly spaced tones), PSK, multi-carrier.

On synthetic signals at 20 dB SNR (`tests/test_features.cpp`) it measures
2-FSK at 45.45/170, 75/850, 100/170 and 50/450, and asynchronous RTTY
with pauses, to within 10 Hz and 0.6 Bd; 8-FSK at 125 Bd and 250 Hz;
MFSK16; BPSK31; 2400 Bd serial PSK; 12 overlapping DQPSK subcarriers 200 Hz
apart (CIS-12's layout); a 16-tone parallel modem; Morse and a carrier; and
reports nothing for noise. It finds the frames of 8PSK modems built like
STANAG 4285 (106.67 ms, also at 3 dB SNR), 4529 (213.33 ms), 4539 (119.58
ms, 31 known symbols in 287) and CIS BPSK 1200 (426.67 ms), and none for
random 8PSK, BPSK31, FSK, MFSK, the CIS-12 layout, Morse or a carrier. These are measurements: the UI shows them as
"≈ 2-FSK, shift 850 Hz, 75 Bd" for emissions no decoder recognised, never
as an identification. They are kept in the log (`emissions.features`,
JSON) and in the exports.

Scored on the Signal Identification Wiki's samples (2026-11-10; the band
found from each sample's spectrum, the first 8 s measured, against the
modulation the wiki's page names and the rates and shifts in file names):
of the samples of FSK pages, 149 of 247 measure as FSK (105 before the
rules above; PSK pages as PSK 123 of 203, unchanged); the 17 RTTY samples
named by shift and rate have the shift right in all and the rate in 11
(none before; the rest are 85 Hz shifts and 100 Bd at 170 Hz, where the
tones are barely apart); of 51 FSK and MFSK samples whose name gives a
rate, 34 are within 3 % (20 of 45 before). MFSK pages measure as MFSK in
70 of 152, multi-carrier pages as multi-carrier in 20 of 88: those are the
weak spots now.

### Consistent with: the catalogue

`Catalogue` (`catalogue.hpp`) compares the features with the `match`
blocks of `catalogue/modes.json` (embedded at build time): the family must
be the same, and every parameter both sides have must agree (shift within
7 %, rate within 3 %, spacing within 6 %, tones within the block's range or
2 of a listed count). A frame period within 2 % of the catalogued one
counts for the mode, and never against it (a modem also repeats in its
preamble, an idle pattern, or over several frames); when it agrees, a rate
that does not is taken as one of the frame's lines and counts as
unmeasured. The modes found are ranked by the parameters matched, then
frame matches first, complete before partial, then by provenance
(documented, measured, vendor, observed, guess; a match takes the weakest
provenance of the parameters that agreed), and the first five go with the
features as `consistent`. The UI lists them as "consistent with" for
emissions no decoder recognised, with "frame matches" where it did. It is
not an identification: several catalogued modes often share every
parameter that is measured (SITOR-A, DSC-HF and CODAN Selcal are all
100 Bd / 170 Hz FSK). The frame tells apart the serial modems whose frame is
catalogued: STANAG 4285 (106.67 ms), 4529 (213.33 ms), 4539 (119.6 ms),
MIL-STD-188-110 serial (66.67 or 200 ms) and CIS BPSK 1200 (426.67 ms); not
3G/4G ALE, Link-11 SLEW or HFDL, whose frames are not catalogued. Their
sync patterns are the rest of layer 2 (milestone 5).

### Known frequencies

`FrequencyList` (`frequency_list.hpp`) holds what is listed as transmitting
where: the GMDSS DSC and NAVTEX frequencies (built in, from the ITU), and
EiBi's schedule (built in, `catalogue/eibi/`; replaced by a `sked-*.csv`
of the user's), and the lists the user puts in the `frequencies` folder
of the data folder (`--frequencies DIR`), read at start and again when the
UI asks. An entry keeps how to listen to it (the list's demodulator, or
what its mode implies) and, from EiBi, its hours on the air: lookups for
an emission leave out entries that are off the air at its start. It reads what such lists look like
without needing to know whose they are: lines of text with the frequency
first (commas, semicolons, tabs or spaces), and JSON with entries as arrays
starting with the frequency or objects with a `frequency`. One unit per
list, from its largest number (Hz, kHz or MHz). KiwiSDR's own label file
(`dist.dx.json`, 872 entries) reads completely; of its labels, 229 name a
mode of the catalogue (129 HFDL, 32 DSC, 22 NAVTEX ...).

A list is evidence, never an identification. An emission whose band holds
a listed frequency (150 Hz of slack), or lies up to 2.5 kHz above one
(lists often give the dial frequency of a USB receiver), gets the entries
with its features ("listed on this frequency"); a catalogued mode the list
names goes first among the modes the measurements are consistent with; and
a decoder the list names is tried from the start, whatever the measured
bandwidth: SITOR-B, DSC, CW, PSK, and RTTY at the listed rate and shift.
The decoders' own gates still decide what is believed.

### HFDL

`hfdl.cpp` decodes the aircraft data link of ARINC 635: bursts of 1800 Bd
BPSK, QPSK or 8PSK on a 1440 Hz subcarrier. A burst is found by its
preamble (a 127-symbol sequence sent twice; correlated in 16 pieces, so
that a tuning error of 100 Hz does not hide it), the next 127 symbols name
the bit rate and the length, and the known 15 symbols after every 30 data
symbols give the channel's phase and level, interpolated over the data.
Then: unscrambling, the 40-row deinterleaver, a soft-decision Viterbi
decoder (K = 7, rate 1/2; at 300 bit/s every coded bit arrives twice), and
the frame check of the PDU's header. Only PDUs that pass are reported: who
sent what to whom (ground station by name, aircraft by its link ID), the
bit rate, the kind of each LPDU, and the 24-bit aircraft address where a
log-on or log-off carries it. The contents of data LPDUs (ACARS text,
position reports) are not read yet.

Where it runs: on every channel a frequency list names for HFDL inside
the receiver's band (at most 16), all the time, like the FT8 sub-bands;
and on any emission 1800 to 3600 Hz wide. The first matters: played as
unlisted emissions, the wiki's recordings gave 2 of their 21 bursts (the
detector merges and splits bursts that follow each other closely, and a
session starts too late for a burst's preamble); as a listed channel, 19.
The two misses are in the longest recording (one a QPSK burst): there is
no equaliser yet, so multipath hurts, the higher rates first.
8PSK (1800 bit/s) is tested only against the file's own modulator.
The protocol's constants are as dumphfdl has them (GPL: read, not copied).

### Radiofax

`fax.cpp` receives pictures sent as radiofax (weather charts, press
photos): the frequency of the audio is the shade, black 800 Hz below
white, one line after another. It demodulates the frequency, takes black
and white from the signal itself (the 3rd and 97th percentile, so that a
receiver tuned a few hundred hertz off, or a station with the narrow
shift of long wave, still gives a full picture), and looks at how the
signal repeats: a fax repeats after one line (0.5 s at 120 lines a minute;
60, 90 and 240 are tried too) and not in between. After 20 s of that the
emission is named FAX, unless its level goes up and down within the
second (a keyed carrier, a time signal's pulses), its frequency goes well
below black (the sync pulses of SSTV) or it also repeats after a third or
a fifth of that line (SSTV's shorter lines): the first version named three
SSTV samples and a time signal of the wiki a fax at 60 lines a minute.
Random two-tone FSK on the same two frequencies, noise and a steady tone
are not named either (tests).

The picture: the line's true length is measured from how the picture
repeats 1, 10 and 100 lines later, because a clock that differs by one
part in a thousand slants a chart by its whole width in 500 lines (a
recording of the wiki, off by 0.4 %, comes out straight). Phasing lines
(one shade with a short pulse of the other), when received, set where a
line begins; otherwise the picture is cut where reception began. A stop
tone (450 Hz), a new start tone (300 or 675 Hz, which also gives the index
of cooperation) or the end of the emission ends the picture; it is written
as a PNG (the program's own writer: rows as differences from the row
above, run lengths) into the `pictures` folder and shown in the window
with the emission's text. 30 minutes at most per picture.

Limits: nothing under 20 s is named; the picture of an emission the
detector splits (deep fades) comes in pieces; the index of cooperation is
576 unless a 675 Hz start tone was received; no noise filtering.

### SSTV

`sstv.cpp` receives amateur slow-scan television: frequency modulation as
radiofax, with sync pulses at 1200 Hz below the picture's 1500 to 2300 Hz,
and a header (VIS) that names the mode with seven bits and a parity bit.
Decoded: Martin M1 and M2, Scottie S1, S2 and DX, Robot 36 and 72, PD 50
to 290 (line timings of N7CXI's specification). A picture is taken up at
its header (parity right, a code of one of these modes), or, when
reception began inside it, from at least six sync pulses one line apart:
only for Martin, Scottie and PD, whose line times are their own (Robot's
are shared by modes not decoded), and only when the pulses have the
mode's length and the 300 Hz between sync and black are nearly empty (a
picture received with the sidebands swapped has neither, and is left
alone: the first version made a wrong picture of the wiki's one). The
line's length and start are fitted to the sync pulses found, the tuning
error taken from the header's tone or the sync's frequency, so a clock
0.3 % off and a receiver 120 Hz off give the same picture (tests).

Limits: Robot 8, 12, 24 and the black-and-white modes are not decoded;
a picture that is mostly black is detected as a narrow carrier at its
black level and gets no SSTV decoder; a header lost while the session
starts over leaves a Robot picture unnamed.

### Time signals

`timesig.cpp` watches the level of the carrier (within about 50 Hz of the
strongest frequency in the session's first second) and of the whole
channel, 100 times a second. A time station is taken to be anything whose
level, folded over exactly one second, has a clear shape, while folded over
0.95 or 1.05 s it has none, for 12 s. That finds DCF77, MSF, RBU, RWM, CHU,
JJY, WWVB and WWV in the wiki's recordings (13 of 20 recordings); BPM's and
TDF's are missed (BPM's 10 ms ticks inside its voice and Morse, TDF's
phase modulation). Morse, steady carriers and noise are not taken (tests,
and none of the wiki's other entries).

DCF77 and MSF send the date and time once a minute. DCF77: the carrier
drops to 15 % for 0.1 s (a 0) or 0.2 s (a 1) at each second, and not at
second 59, which marks the minute; MSF: the carrier is off 0.1 s at each
second, 0.1 s more for bit A and again for bit B, and 0.5 s at second 0. A
minute is reported only if all its checks pass: DCF77's three even
parities, bit 20, the summer and winter bits; MSF's four odd parities on
bits B and its fixed 01111110 on bits A 52-59; and for both, BCD digits
in range, a real date and the weekday that date has. The time is the one
the station sends (CET/CEST, UTC/BST). JJY's and WWVB's codes are not
decoded: no recording of a whole minute of either to check them on.

### WSPR

`wspr.cpp` decodes WSPR beacons in the sub-band at each WSJT-X dial
frequency inside the receiver's band, one 2-minute slot at a time, on
the FT8 worker thread (about 0.15 s of CPU per slot and band). It looks
for the 162-symbol sync vector over the 200 Hz sub-band (in steps of
half a tone, 0.73 Hz) and the slot's start (in quarter symbols), then
decodes the convolutional code (K = 32) with a sequential stack decoder.
WSPR has no checksum, so a message is believed only if its callsign has
a callsign's form, its locator is on the grid, its power is one of the
allowed values, and, coded again, it agrees with at least 75 % of the
strongest received tones. Standard messages only (type 1): compound
callsigns and 6-character locators are not decoded. Synthetic slots
decode down to about -25 dB SNR in 2500 Hz (WSJT-X's decoder reaches
about -28 to -31 dB); the reported SNR is an estimate.

### DRM

`drm.cpp` recognises DRM by the guard interval of its OFDM symbols: the
end of every symbol's useful part is sent again before it, so the signal
correlates with itself one useful-part length later, during the guard
interval of every symbol, at every carrier. The detector takes the audio
to complex baseband (a correlation of real audio can cancel out, as the
channel's frequency shift turns its phase by an arbitrary angle), folds the
correlation at each robustness mode's useful length (24, 21.3, 14.7 or 9.3
ms) over the mode's symbol length, and names the mode when the guard
interval's peak stands out in three 2 s windows in a row. Since any slice
of the carriers shows it, a 2.8 kHz session of a 10 or 20 kHz broadcast
is enough. Emissions wider than 3.6 kHz, for which there was no decoder,
now get a session for this; and such an emission keeps its session when
its measured width wavers around 3.6 kHz (it used to start over and
over). Signals narrower than 3.2 kHz are labelled "DRM-type OFDM": the
amateur WinDRM and EasyPal use DRM's modes, and other systems may (it
labelled the wiki's "Russian MFSK-OFDM-chirp hybrid modem", whose OFDM part
I cannot check). The broadcast's programme is not decoded.

### What the processing goes to

The decoder host's policy (`DecoderHost::Config`, changeable while
running with `setPolicy`): at most `maxSessions` emissions at once; an
emission no enabled decoder fits is measured once (`measureUnknown`) and
its session then ends; when the sessions are full, an emission with
decoders takes the place of one that is only being measured; a session
whose decoders have read nothing `giveUpSeconds` after it started (and
after it was measured) ends, and its emission gets no new one; emissions
below `minSnrDb` get none; `disabled` decoder families are not started,
and those running end. `ignore(id)` leaves an emission on request. The
time each piece of work takes is measured with the steady clock (each
decoder per block, the meter, the channels' audio, the FT8/FT4 and WSPR
worker, the HFDL channels; the spectrum and detector as the rest of the
pipeline's time) and reported by `usage()`, with what each session is
doing. Timing costs about 30 ns per decoder and block.

### The learned family model

`FamilyModel` (`family_model.hpp`) is a random forest of 40 trees over a
vector of 86 numbers that the feature meter computes for every emission
(`EmissionFeatures::vector`: the spectrum's shape, where the instantaneous
frequency dwells, the amplitude's distribution, a fourth-order cumulant,
lines of the squared and fourth-power signal, and the meter's own
measurements). It is trained offline on the Signal Identification Wiki's
samples (`tools/ml/`), embedded as text (`catalogue/family_model.txt`), and
run without any library. It gives the most probable of seven families (FSK,
MFSK, PSK, multi-carrier, OOK, sweep/pulse, analogue) and its probability,
and names it only at 0.5 or more; otherwise it is "not sure".

It is a prediction, and the program keeps it apart from what it measures:
the features carry it as `predicted` (family, confidence, sure, model
version), the UI shows it as "Model's opinion". It does not change the
measured family, the consistent modes or which decoders run, and it never
names a system. On wiki pages held out of training, models of this kind
name the right family for 59 % of signals (the rule-based meter: 44 %),
and for 87 % of the 44 % they are sure of; `tools/ml/README.md` has the
protocol, the comparison with other models and the limits.

### Measured sensitivity (synthetic signals, white noise, no fading)

Lowest SNR at which the test message decoded, two noise seeds each,
through the whole chain (IQ at 62.5 kS/s, channelizer, detector, decoder
host). SNR is signal power over noise power in 2.5 kHz. Measured in steps
of 3 dB down to -3 dB (FT8 to -24 dB), so "-3 dB" means "at least as low as -3 dB".

| Mode | Lowest SNR decoded |
|---|---|
| FT8 | -22 dB |
| PSK31, CW (22 WPM), RTTY 45.45/170, NAVTEX | -3 dB |
| ALE 2G | 0 dB (one of two at -3 dB) |

These are not figures for real HF: fading, interference and real
transmitters are not in them. Recordings (milestone 7) will give those.

## What the API gives and what this code adds

Documented by SDRplay (WebSocket API 1.0.3):

- JSON text messages `{event_type, property, value, device}`; binary messages
  with a 2-byte type: IQ is type 2 (primary) and 5 (secondary), signed 16-bit
  interleaved.

Not documented, measured by a third party on an RSP1A (RESEARCH.md 1.2):

- IQ arrives at `device_sample_rate`, about 20 messages a second, within 1 %
  from 250 kS/s to 2 MS/s;
- a refused setting (3 MS/s) gets no answer at all, and so does a setting
  that changes nothing;
- a slow reader gets about 2.5 s of buffering, then SDRconnect drops IQ
  without closing the connection.

What this code does about it:

- **Refused settings.** `setAndConfirm` waits for `property_changed`; if
  none comes it asks with `get_property`. The caller compares the answer
  with what it asked for.
- **No timestamps.** `IqClock` numbers samples, dates each block from its
  arrival time and the reported rate, and detects lost IQ: when samples are
  lost, the blocks arrive late *for good* (their lateness stays up, the same
  for every block); when the reader merely stalls, they arrive in a burst
  whose lateness falls as it catches up. So a gap is the minimum lateness
  over a whole second of arrivals rising above its baseline by more than
  0.2 s, *and* the lateness steady over that second. The gap is then placed
  at the first block that arrived late, and the UTC estimate re-anchored.
  Tested with stalls of 0.5 to 2.4 s (SDRconnect buffers about 2.5 s) and
  with samples lost during a stall.
- **The reported rate, not the measured one**, is what IQ is processed and
  recorded at (`IqBlock::nominalRate`): the measured rate varies from block
  to block. IQ that arrives before SDRconnect has reported its rate is not
  used.
- **Retunes.** IQ carries no tuning information, and IQ that arrives just
  after a retune may still be from before it. Blocks that arrive within
  0.1 s of a `device_center_frequency` change are flagged `settling`.
- **Slow readers.** Handlers run on the reader thread and must return fast;
  `sdrc_record` queues and writes on another thread.

## Parameters

| Parameter | Value | Where |
|---|---|---|
| Gap threshold | 0.2 s | `IqClock::Config::gapThresholdSeconds` |
| Gap detection window | 1.0 s (detection lags a gap by up to this) | `gapWindowSeconds` |
| Measured-rate window | 5 s | `rateWindowSeconds` |
| Retune guard | 0.1 s | `guardSeconds` |
| Clock-drift tracking | 60 s time constant | `baselineTimeConstant` |
| Largest WebSocket frame accepted | 64 MiB | `WsFrameDecoder` |

| Detector frame | 0.128 s | `Detector::Config::frameSeconds` |
| CFAR threshold | 7 dB above the floor | `thresholdDb` |
| Weak wide regions: median window, threshold, width, strongest segment | 180 Hz, 3 dB, >= 600 Hz, < 15 dB | `wideSmoothHz`, `wideThresholdDb`, `wideMinHz`, `wideMaxPeakDb` |
| Confirmation / hold | 3 frames / 3 s | `confirmFrames`, `holdSeconds` |
| Grouping: onset, gap, SNR difference | 0.4 s, 1100 Hz, 10 dB | `groupOnsetSeconds`, `groupGapHz`, `groupMaxSnrDiffDb` |
| Decoder sessions at once | 48 | `DecoderHost::Config::maxSessions` |

## Limitations

- **Not yet run against a real SDRconnect.** Tested against the fake
  SDRconnect in `testing/`, which follows the API document and the
  third-party measurements; `tools/sdrc_probe` is how the real behaviour on
  a PC gets measured.
- **Absolute UTC accuracy** is limited by SDRconnect's processing delay,
  which is unknown and not corrected. A guess, not a measurement: tens of
  milliseconds. Relative timing inside a stream is exact (sample counts).
- **Small gaps go unnoticed.** Losses shorter than the 0.2 s threshold are
  not detected; the arrival-time jitter of ~20 messages a second does not
  allow less. Losses between 0.2 s and 1 s are detected up to 1 s late; the
  blocks in between carry UTC times that are off by the loss.
- **Little-endian samples** are converted explicitly, so any host works;
  only x86-64 Linux and Windows (MinGW cross-build; MSVC in CI) are built.
- **Retunes made in SDRconnect** are known only when SDRconnect reports
  them (third-party measurements: about 0.5 s later). IQ in between is
  processed as if at the old frequency. Retunes made by this program
  (*Centre receiver here*) mark the IQ as settling from the moment they are
  asked for.
- **The first ~0.4 s of an emission is not decoded**: a session starts when
  the emission is confirmed (3 frames). A CW call may lose its first letter;
  NAVTEX phasing, PSK preambles and ALE scanning calls cover it.
- **Grouping by onset is a heuristic.** Two unrelated signals that start
  within 0.4 s, less than 1.1 kHz apart and at similar levels become one
  emission (outside the FT8/FT4 sub-bands; not in WSPR or JS8 sub-bands
  either, which are not excluded). Tones of one signal that start apart are
  separate emissions until they overlap.
- **A burst over a longer emission is taken as part of it.** The tracker
  gives each segment to the emission it overlaps most, so a wide burst
  that overlaps a narrower signal already on the air, even partly, widens
  that emission for a moment instead of becoming its own: its decoders
  never run. Measured on the wiki's one real 3G BW0 burst (7 dB): it is
  plain in the spectrum (0.6 s, 4 to 7 dB above the floor across 700 to
  2900 Hz), but it overlaps a 0.9 kHz signal in the same recording (or,
  with less noise added, the recording's own noise passband) and is
  absorbed into it.
- **The power-weighted centre** of an FSK emission leans towards the tone
  sent more often; the middle of the occupied band (`occLoHz`, `occHiHz`) is
  the better centre there, and is what ALE sessions use.
- **Unknown text is not decoded**: see the plausible-text rule above.
- **The decoders' figures are from synthetic signals** (each decoder's own
  generator, through IqScene, with white noise); no fading. The only real
  signals so far are sigidwiki's audio samples (`tools/sigidwiki_bench.py`,
  results in the README): short, mostly strong clips, made into IQ.
- **3G and 4G ALE PDUs do not decode on sigidwiki's real samples, because
  they are protected**: the three real bursts (Deep WALE and Fast WALE at
  19 dB SNR, 3G BW0 at 7 dB) arrive as exact code words (no received coded
  bit off the code word decoded), which confirms the waveforms, scramblers,
  interleavers and codes on independent signals, but their CRCs fail; the
  WALE PDUs' under every bit order, tail-biting rotation and CRC-16 variant
  tried (about 43,000), and they form no link setup header. That is what
  linking protection produces: WALE encrypts each PDU (HALFLOOP-96) before
  coding. The NAVTEX plugin's `docs/ALE.md` ("Real recordings") has the
  details. They are reported as "received intact, data encrypted (linking
  protection) or not to the standard"; the PDU layouts and CRCs are still
  checked only against the standards, not against a real unprotected PDU.
  The other 3G samples are data waveforms (BW2-BW4, and 36 s of STANAG 4539
  traffic), which are not decoded at all.
- **Frame periods**: measured only from 10 to 650 ms, and only when the
  signal repeats at twice the period within the 8 s measured (so not on a
  burst shorter than about 1.5 s plus twice its period). A strong steady
  tone in the band (a pilot) raises the correlation at every lag and can
  hide a frame. On the wiki's samples (`docs/RESEARCH.md` section 3.7) the
  frame was found on every STANAG 4285 and 4529 sample and on 19 of 20
  MIL-STD-188-110 serial samples, but STANAG 4539 repeats at 119.6 ms on
  only 2 of 5 (its probes vary, so on others at a multiple of it), and
  CIS BPSK 1200 at 426.67 ms on 1 of 3. 110A-MOD's symbol rate is still
  misread (its envelope has strong lines every 75 Hz); its frame still
  matches the catalogue.
- **Known frequencies**: a list's entry is matched by frequency alone
  (and by its hours, where the list gives them; not by the day of the
  week or the season, which EiBi also gives), and different stations share
  frequencies. Lists give dial frequencies, carriers or centres without
  saying which, hence the 2.5 kHz allowance, which can attach an entry to
  a neighbour. Shipped: the nine ITU frequencies and EiBi's schedule,
  whose terms allow it; the other hobby databases state no such right.
  The shipped schedule is one season's (B26) and goes stale.
- **Akula and the 4:3 code are recognised by structure, on little
  evidence so far.** Akula's groups are those analysts published; the
  program finds them in an Akula recording (two bursts,
  155 words each), but that recording is where they were first checked
  (`docs/RESEARCH.md` section 3.6), so it is no independent test; there is
  no other Akula recording yet. Bursts without the sync group ("data-only"
  ones) are not reported: the end group alone, tried first, turned up in a
  KG-STV sample of the wiki. The 4:3 code names a code, not a system:
  it is found on 4 of the wiki's 5 CIS-36-50 and CIS-50-50 samples, and
  equally on its POL-ARQ, SWED-ARQ, ARQ-E3 and ARQ-N samples, which use
  codes of the same weight (a first version labelled all of them
  "T-600"). T-600's own start and end sequences are not searched for.
  Neither detector reads or keeps any traffic.
- **DSC** is implemented from ITU-R M.493 as remembered, not re-read; the
  four real HF calls among the wiki's samples decode with their check
  characters right, which confirms the code, the phasing and the two
  copies, but the wording of rarer fields (geographic areas, distress
  relays, expansions) is unchecked, and symbols it does not know are
  printed as numbers. VHF DSC (1200 Bd) is not decoded.
- **FT8/FT4 need exact 12 kHz channels**, which every SDRconnect rate
  gives; for a rate that does not (the channelizer then runs slightly off
  12 kHz), FT8/FT4 decoding is off and the other decoders run at the real
  channel rate.
- **FT8/FT4 dial frequencies** are the WSJT-X defaults as listed in
  `ftx_decoder.cpp`, not verified against the WSJT-X source.
- No TLS: SDRconnect's WSS exists only in headless 1.0.10 and is not needed
  on 127.0.0.1.
- **No keep-alive**: if SDRconnect's host vanishes without closing the
  connection, the client waits. Irrelevant on 127.0.0.1; matters only for
  SDRconnect on another machine.
