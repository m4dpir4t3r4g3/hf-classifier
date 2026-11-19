# Road map to 1.0

Written 2026-11-15, after 0.4.0. This is what still stands between the
preview releases (0.x) and a first version that can be called working
(1.0.0). The release job publishes any 0.x version as a prerelease and
1.0.0 as a normal release, so nothing changes in CI for that step.

## Where 0.4.0 stands

Tested on synthetic signals, on the Signal Identification Wiki's audio
samples made into IQ, and against the stand-in for SDRconnect
(`testing/fake_sdrconnect`). CI builds and tests on Linux, under
ThreadSanitizer and AddressSanitizer, and on Windows with MSVC. It also
builds the installer, installs it silently, runs the installed program and
uninstalls it.

**It has never run against a real SDRconnect or RSPduo.** 0.3.0 did not work
with SDRconnect_headless for the only user who tried it, and the cause is
not known. Everything below follows from that.

## Where 0.5.1 stands (2026-11-17)

Run on the user's PC with the desktop SDRconnect and an RSPduo: the
connection works (99.7 % of the IQ, no gaps, nothing dropped). Still no
CW, OOK or FT8/FT4 decodes in live use. Known so far:

- A recording (23 WPM net Morse) was decoded by the Morse
  decoder and thrown away by the plausible-text check; fixed in 0.5.1
  (keyed-tone timing instead of known words). Other recordings may show
  other causes.
- FT8/FT4 were switched off in the user's settings, and SDRconnect ran at
  62.5 kS/s: at that rate only about 56 kHz around
  the centre is seen, with no FT8/FT4 frequency in it.
- SDRconnect_headless finds the radio, then stops answering and ends with
  an access violation (0xC0000005). Not known whether it does so on its
  own; `SDRconnect_headless.exe --websocket_port=5454` alone for a minute
  would tell.

How to look at a recording (made with the window's Record button, SigMF):

```
build/hf_classifier --play recordings/NAME.sigmf-meta --speed 0 --exit --db check.db --http 0
sqlite3 check.db "select decoder, freq_hz, text from decodes"
sqlite3 check.db "select emission_id, centre_hz, hi_hz-lo_hz, mean_snr_db, duty, label, features from emissions"
```

What finds the cause: decode the signal independently (for Morse, the
key-down times from the envelope at the carrier: what was sent), then
feed the decoder alone (bypassing the gates in `core/src/decoder_host.cpp`)
to see whether the decoder reads it and a gate refuses it, or the decoder
does not read it. Recordings stay in `recordings/` (not in Git).

**The SDR PC has no internet.** Everything must work offline. The
frequency lists are downloaded with the Download button on a PC with
internet and the files copied from `%LOCALAPPDATA%\HF Classifier\frequencies`
to the same folder on the SDR PC (what the user does; works as is).

## Requested by the user (2026-11-17)

Done (not released yet; CHANGELOG, Unreleased): 1, 2 and 3. Open: 4 and 5.

1. **Decode windows.** Drag across the waterfall to mark a frequency
   window, then choose the decoder for it (with its parameters: RTTY rate
   and shift, PSK mode...) or "Auto". Inside it, emissions get that
   decoder only, without the bandwidth heuristics. Kept in the settings.
2. **No-processing windows.** The same drag, with "Do not decode or
   classify": emissions inside are shown on the waterfall but get no
   session (saves processor time, keeps out known noise).
3. **Markers with a decoder.** In a marker's dialog, "decode only with":
   a decoder and its parameters; when an emission at the marker has been
   decoded (green), "Use these settings" fills them in from the decoder
   that read it (e.g. "RTTY 50/450", "PSK63").
   Design note for 1-3: one list of rules (lo, hi, decoder or "none",
   parameters), from windows and markers alike; `DecoderHost::startSession`
   consults it before `candidatesFor()`. A decoder the user chose may
   pass a lower gate (its text shown as "chosen by you", not as
   recognised).
4. **Voice.** First recognise it: a voice emission (SSB upper/lower, AM,
   FM) from its envelope (syllables, 3-8 Hz), its spectrum (300-3000 Hz,
   no steady tones) and its sidebands, labelled "Voice (USB)" and so on.
   Then a transcript with **whisper.cpp built into the program** (MIT
   licence; the user's SDR PC has no internet, so nothing may need a
   download):
   - the model ships in the installer as an optional component ("Voice
     transcription", ticked by default); it is too big for the repository
     (over GitHub's 100 MB per file), so the release job fetches it while
     building (the Whisper weights are MIT licensed too); the user's choice
     between `base` (142 MB, weak in Bulgarian and Russian) and `small`
     (466 MB, noticeably better) is still open, `small` recommended;
   - the build must run on processors without AVX2 as well (CPU features
     chosen at run time, not one fixed build), and use the P-cores of
     hybrid Intel processors;
   - run on the demodulated audio of voice emissions, off by default, a
     queue with at most one or two at a time; transcripts stay on this PC;
   - expect modest accuracy on noisy HF SSB and a heavy processor load
     (to measure on the user's recordings);
   - listening to voice traffic other than amateur radio and broadcasting,
     and recording or passing on its content, is restricted by law in many
     countries (secrecy of communications); the user decides what to
     transcribe.
5. **Classify all voice and radio emissions correctly.** Taken as: every
   emission gets at least its family (voice, Morse, FSK, PSK, multi-tone,
   carrier, noise-like) with an honest confidence, measured on the user's
   recordings (Phase 2's figures per family). "All, correctly" cannot be
   promised; the figures will say how often.

## What 1.0 means

1.0 should be a version a person can install on Windows 10/11 next to
SDRconnect and leave running for days. It should:

1. connect to SDRconnect (the desktop program and the headless one) and
   receive IQ without anyone editing files;
2. show the band, decode the modes it claims to decode on real signals, and
   say plainly when it does not know what a signal is;
3. not grow without limit, crash or fall behind on the user's PC at the
   recommended sample rate;
4. be installable without alarming warnings, upgradable from 0.x, and
   documented in English and Bulgarian.

Everything else, such as more modes, the machine-learning layer and system-level
classes, can come after 1.0.

## Phase 1: real hardware (blocker, needs you)

Nothing else matters until this works, and only a PC with SDRconnect and an
RSPduo can show it.

| Item | Who | State |
|---|---|---|
| Why SDRconnect_headless did not work with 0.3.0: banner text, SDRconnect version, `SDRconnect_headless` started by hand and its output, `hf_classifier.log`, `sdrconnect_headless.log` (0.4.0 writes both) | you, then me | owed |
| Run against the SDRconnect desktop program as well as the headless one | you | not tried |
| `sdrc_probe` report: which sample rates arrive in full, what dual-tuner mode allows, antenna names. The defaults depend on it | you | owed since 0.1 |
| *Listen in SDRconnect* puts a USB signal in the middle of the passband (if it sounds about 1.5 kHz off, the VFO assumption in `app/README.md` changes) | you | not tried |
| CPU and dropped samples at the sample rate you use, on a busy band (the Resources window shows it) | you | not tried |
| A diagnostic report that collects the logs, versions, probe measurements and CPU load in one click (the probe runs on the program's own connection) | me | done, not released (CHANGELOG, Unreleased) |
| Fix whatever the above shows, with a test against the fake for each behaviour found, and the fake updated to match the real SDRconnect | me | waits on the above |

Exit: 0.5.0 connects, follows SDRconnect, decodes FT8 and NAVTEX on the
air, and both of us have seen it.

## Phase 2: accuracy on real signals

The decoders' figures so far come from synthetic signals and short wiki
clips. 1.0 needs figures from live reception.

- **Your IQ recordings** (`sdrc_record`) of the modes in `docs/RESEARCH.md`
  section 3.5, with frequency, date and what you think they are. They are
  the data for milestone 7.
- **Milestone 7**: per-class precision and recall, and confusion matrices, on
  those recordings and on a few days of live log. Decodes checked by a code
  or CRC (FT8, FT4, ALE, DSC, HFDL) label the data without
  hand work.
- **Fading**: signal generators with fading (Watterson channel) for the
  tests and the benchmark. Today everything is tested in white noise only.
- **Decide which known misses matter for 1.0**:
  - a short burst over a longer signal is absorbed into it (`core/README.md`,
    Limitations). This affects ALE 3G bursts and is worth fixing;
  - the first ~0.4 s of an emission is not decoded, so a CW call can lose its first letter;
  - PSK10 and the PSK FEC variants; 3G data waveforms BW2-BW4 (later);
  - the definitions still missing (`docs/PLAN.md`, "What is still needed", item 2) for the modes they name.
- **Check what was written from memory**: the FT8/FT4 dial frequencies against
  the WSJT-X defaults (facts only), and DSC's rarer fields against
  ITU-R M.493.

Exit: a written report with per-mode figures on real signals, and every
claim in the README backed by it or removed.

## Phase 3: running for days

- **Log size**: the SQLite log grows without limit. Add a retention setting
  (keep N days, or N MB) with the old rows deleted in the background.
- **Soak test**: 72 h on your PC with both tuners, watching memory, CPU, log
  size and dropped samples. A shorter version (hours, against the fake) can
  go into CI or run by hand before each release.
- **Recovery**: SDRconnect restarted, the radio unplugged and plugged in
  again, the PC asleep and woken. Each should end in the program receiving
  again without a restart. Some of this is tested against the fake now. It
  needs checking for real.
- **Speed**: if Phase 1 shows a busy band at 8-10 MS/s does not keep up, a
  faster FFT than KISS FFT (licence to be checked before choosing).
- **UTC delay**: measure SDRconnect's processing delay against a time signal
  (DCF77/MSF are decoded now) and correct for it, or state the measured
  figure.
- **Keep-alive** on the SDRconnect connection, if SDRconnect on another
  machine is in scope for 1.0. If not, say that it is out of scope.

## Phase 4: distribution

- **Licence of this project**: there is no LICENSE file. It has to be
  chosen before a 1.0 others are invited to use. It must fit the code
  copied from the author's SDRuno plugin repositories and the third-party code
  (ft8_lib MIT, KISS FFT BSD, SQLite public domain). Your decision.
- **Code signing**: the installer and the programs are unsigned, so Windows
  SmartScreen warns on every download. A code signing certificate (yours)
  and a signing step in the release job would fix it. Without one, the
  README must say what the warning is and how to get past it.
- **Upgrade from 0.x**: install 1.0 over 0.3.0 and 0.4.0 in CI, checking
  that settings, markers and the log carry over (the log upgrade has a
  unit test; the installer path does not).
- **Data that goes stale**: the shipped EiBi schedule is season B26. 1.0
  should say when its lists are old and offer the download button.
- **New version notice** (optional): tell the user when a newer release
  exists on GitHub, off by default or asked once.

## Phase 5: documentation and polish

- A short user guide (EN/BG): install, connect to SDRconnect, what the
  windows show, what the labels mean and how sure they are.
- A troubleshooting page built from what Phase 1 finds: which log to send,
  what each connection state means.
- The web UI has no automated test. Add a few browser tests (both languages,
  phone width, the main windows) in CI with Playwright and Chromium.
- Go through the README and CHANGELOG and remove every "not yet tested"
  that 1.0 has made true, and keep the ones that are still true.

## Release steps

| Version | Contents |
|---|---|
| 0.5.0 | Phase 1 fixes: works with a real SDRconnect |
| 0.6.0 | log retention, recovery, the first figures from real recordings |
| 0.9.0 | feature freeze: licence, signing (or the documented warning), upgrade test, user guide |
| 1.0.0-rc | a week of use on your PC with no new bug that matters |
| 1.0.0 | published as a normal release |

The version numbers are a suggestion. What matters is the order: real
hardware first, then accuracy and stability, then distribution.

## Not needed for 1.0

Milestone 4-6 items still open (grouping by features, T-600 and STANAG
4285 sync patterns, "similar to" for unknowns, system-level classes, the ML
layer trained on your data), more decoders (VHF DSC, 3G data waveforms,
PSK FEC variants), TLS, Linux packaging, and other SDR software than
SDRconnect.
