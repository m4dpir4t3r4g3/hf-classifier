# Changelog

## 0.6.0 (2026-11-17): fifth preview release

Still a preview. With 0.5.1 on a user's PC (the desktop SDRconnect and an
RSPduo, receiving without loss) there were still no CW, OOK or FT8/FT4
decodes live; the cause is being looked for in the user's recordings and
is not fixed in this version. This version lets you tell the program what
to decode where, and what to leave alone. SDRconnect_headless still ends
with an access violation of its own after opening the radio.

Your own decoding rules
- New: **frequency windows.** Drag across the waterfall to mark one, then
  choose what happens to the signals whose centre is in it: one decoder
  only (with RTTY's rate and shift), used without the usual guesses from
  the bandwidth and without the check that its text reads like radio
  traffic; or **no processing** at all (no decoding, no classification,
  and no FT8/FT4/WSPR or HFDL channel inside it). Where windows overlap,
  the narrowest decides. Shown over the waterfall (click a window's label
  to change or delete it) and listed in Settings → Processing. Kept in
  `decode_windows.json` in the databases folder, so it travels with the
  lists to a PC without internet.
- New: **a marker's decoder.** "Decode only with" in a marker's dialog:
  signals within 1.5 kHz of the marker get only that decoder. When the
  selected signal has been decoded (green), the decoder that read it is
  filled in.

Fixed
- Saving a marker (or a window) on Windows while the file was open in
  another program was reported as done though nothing was written; it is
  now reported as failed.

Tested: on synthetic signals, against the stand-in SDRconnect, in Chromium
(drawing, changing and listing windows, a marker's decoder filled in for
every decoded mode of the demo), and by CI on Windows (build, installer,
silent install). Not yet with a real receiver.

## 0.5.1 (2026-11-17): fourth preview release

Still a preview. 0.5.0 ran with a real SDRconnect (the desktop program,
API 1.0.3) and an RSPduo on one user's PC, receiving without loss (99.7 %
of the IQ, no gaps, nothing dropped), and decoded nothing: a recording
made with it showed why (below). SDRconnect_headless found the radio and
then ended with an access violation of its own; that is not solved.

Decoding
- Fixed: **real Morse traffic that was not amateur radio was never shown.**
  A user's recording (23 WPM, 10 to 15 dB over the noise) was
  read by the Morse decoder ("E RPTK E E E E E TEJ R 853K", confidence
  0.7 to 1), then refused by the check that keeps other modes' signals
  from being labelled CW: it wanted callsigns, "CQ" or known words, and
  took the many single E's for noise keyed as Morse. When an emission is
  measured as a keyed tone (OOK) and its key-down times have the timing
  of Morse (dots, dashes about three times longer, next to nothing at two
  units or at five and more, measured apart from the decoder), confident
  Morse now passes without known words, provided its words are short as
  people's are. That recording now decodes. A test plays such Morse next
  to a carrier in deep fading and to random on-off keying at 8, 12 and
  20 Bd: the Morse decodes, the others give nothing (also checked with 16
  random sequences).

SDRconnect headless
- After headless reports the radio, the program waits 3 s before setting
  it up: on a user's PC headless stopped answering right after reporting
  the radio and then ended with an access violation (0xC0000005); whether
  a question at that moment caused it is not known.

Window server and files (made with GitHub Copilot, reviewed)
- At most 64 connections to the window's server at once (more are
  refused), and 5 s for a request's whole header, not for each read.
- Fixed: stopping could hang on a browser that no longer read.
- Exactly one Host header; WebSocket version 13 only; text messages from
  browsers must be UTF-8 (not those from SDRconnect: a name in a Windows
  code page must not cost the receiver its connection); "%00" stays text in
  addresses.
- Pictures and reports are served only as plain names of regular files
  up to 64 MB; ZIP files are written only within their 4 GiB limit and read
  only with names that stay in their folder.
- Fixed: two diagnostic reports made in the same second had the same name,
  and the second replaced the first.

## 0.5.0 (2026-11-16): third preview release

Still a preview. A test build of 0.4.0 with the diagnostic report below
connected to a real SDRconnect (the desktop program, API 1.0.3) and an
RSPduo on one user's PC: 97.9 % of the IQ arrived at 2.048 MS/s, without
gaps. That report also showed what this version fixes: the processing
falling behind on a busy band, and SDRconnect_headless answering
"Refreshing..." and nothing else. 0.5.0 itself has not yet run with a
real receiver; the headless fix is tested only against a stand-in that
imitates what that report showed. Everything else is tested on synthetic
signals and against the stand-in SDRconnect (`testing/`), and by CI on
Windows (the installed program, its diagnostic report, its log).

Processing
- Fixed: **0.4.0 fell behind on a busy band and dropped the signal**, so
  that it decoded far less than 0.3.0 did (a user's report: 0.3.0
  decoded FT8/FT4 and CW at once, 0.4.0 not even one CW signal; its
  diagnostic report showed 431 million samples dropped at 2.048 MS/s). Its new decoders (HFDL channels decoded all the time, radiofax,
  SSTV, time signals, DRM tried on many emissions) made it about 40 %
  slower, on one processor core. The decoding sessions now run in
  parallel on a pool of threads (the processors less one, at most 16 per
  receiver); their results are handed on in the same order as before, so
  nothing else changes. On a synthetic busy band (make_busy_scene,
  2.048 MS/s): 30 s took 37 s before, 17 s now on 4 processors (0.3.0:
  26.6 s); the same decodes.
- Uses large processors better: up to 16 decoding threads per receiver
  (was 8); on Windows the program asks not to be put into "efficiency
  mode" (EcoQoS), which Windows 11 gives programs without a visible window
  and which keeps them on the efficiency cores of hybrid processors (Intel
  12th generation and later); the diagnostic report says whether Windows
  took it. Not tried on such a processor here.
- Less work for the same result: the HFDL and time-signal decoders mixed
  with a sine and a cosine per sample (now a rotating phasor); the feature
  measurement made new FFT plans every time (now shared); the detector
  tried every segment against every tracked emission (now only those that
  can overlap it). Same emissions and decodes on the synthetic busy band;
  its 30 s took 22.5 s before these, 20.1 s after, on the 4-processor
  development machine. Of that, 8.9 s run on one core (the spectrum and
  the detector), the rest on all of them.
- The window warns when the processing falls behind and signal is
  dropped (it said nothing before).
- When every session is taken, the undecided one tried longest (at least
  20 s) makes room for a waiting emission; before, the first emissions on
  a busy band kept the sessions for the give-up time (120 s) and later ones
  were never tried. On the synthetic band this neither helped nor hurt; a
  busy band still has more emissions than sessions (on it, broadcasters'
  sidebands take many), which costs decodes in both versions.
- New tool `make_busy_scene`: a synthetic busy band to measure that with.

Window
- The Resources window is gone; what it showed is where it is used. The
  top bar shows the load ("Load N % · M decoding": how much of real time
  the processing is busy; green, orange from 80 %, red while signal is
  dropped, replacing the separate warning), and a click on it opens the
  time per activity. What happens to each signal shows in the emissions
  table and in its details, with a button to stop processing it and a
  filter "Being processed". The settings are in Settings → Processing,
  in effect as soon as saved. (The Resources window's number fields had
  also lost what was typed into them every 2 s.)

SDRconnect headless
- SDRconnect_headless, just started, answered "Refreshing..." for the
  active radio and nothing else (a user's diagnostic report); the program
  went on as if connected, with "Refreshing..." as the radio's name, and
  nothing came. It now waits while headless looks for radios (up to 30 s,
  saying "SDRconnect is looking for receivers"), opens the radio itself
  if none is open (from valid_devices, not the IQ file player, an RSPduo's
  single-tuner entry first), logs each step, and says when it gets no
  radio. Not yet tried with the real SDRconnect_headless: that report is
  the only evidence of how it behaves. The fake SDRconnect imitates it
  (`--refreshing S`, `--no-device`).

Diagnostics
- Fixed: HFClassifier.exe's log was empty for anyone reading it while the
  program ran (a user's report had it empty; CI on Windows found the
  same), so reports lacked the current run's messages. Its messages now
  go through a pipe the program reads itself, written to the file at once
  and kept in memory (the last 2 MB) for the report.
- The version of SDRconnect's files also gives their product version
  text, since the file version reads 1.0.0.0.
- New: **diagnostic report**. One click makes one ZIP file with
  everything needed to see why the program does not work on a PC: the
  program's, Windows' and SDRconnect's versions, processor and memory, the
  settings, the connection state and processing load, where SDRconnect is
  installed, whether it runs and whether its port answers, the
  measurements `sdrc_probe` makes (on the program's own connection while
  it runs), and the logs. From Settings, the window's warning when the
  connection fails, the icon's menu, the Start menu ("HF Classifier
  diagnostic report", which works even when the window does not open), or
  `hf_classifier --report`. No recordings or log of signals; README.txt in
  it says what it contains. The window offers it for download.
- `sdrc_probe`'s measuring is now in the core library, shared with the
  report; its report file keeps the measurements under `"probe"`.
- Fixed: starting HFClassifier.exe a second time (the shortcut clicked
  again, which only opens the running one's window) emptied the running
  one's `hf_classifier.log`. The log of the run before is now kept as
  `hf_classifier.previous.log`.

Tested against the stand-in SDRconnect on Linux, and by CI on Windows (the
installed program makes a report with the stand-in answering, and the
running program is asked for one); not with a real SDRconnect.

## 0.4.0 (2026-11-15): second preview release

Still a preview. **It has not yet run against a real SDRconnect or
RSPduo**: everything below is tested on synthetic signals, on the Signal
Identification Wiki's audio samples made into IQ, and against a stand-in
for SDRconnect (`testing/`). 0.3.0 did not work with SDRconnect_headless
for one user and the cause is not known yet; this version logs what
happens (below), so a report with `hf_classifier.log` and
`sdrconnect_headless.log` from the data folder shows where it stops.

Decoding
- New: **HFDL** is decoded (ARINC 635, the aircraft data link): ground
  station squitters, uplinks and downlinks at 300 to 1800 bit/s, with the
  kind of each LPDU and the aircraft address on log-ons. Only PDUs whose
  frame check is right are reported. Channels a frequency list names are
  decoded all the time (EiBi names 124). 19 of the 21 bursts in the
  Signal Identification Wiki's five recordings decode.
- New: **radiofax** (weather fax): recognised by its lines repeating, at
  60, 90, 120 or 240 lines a minute; the picture is saved as a PNG and
  shown with the emission, straightened (the line length is measured from
  the picture) and aligned when the phasing lines were received. The four
  recordings of the Signal Identification Wiki longer than 25 s give
  their pictures.
- New: **SSTV**: Martin M1/M2, Scottie S1/S2/DX, Robot 36/72 and PD 50 to
  290, named from the picture's header or, inside a picture, from the
  timing of its sync pulses; the colour picture is saved as a PNG and
  shown with the emission. 9 of the Signal Identification Wiki's 14
  recordings give their pictures (the others are modes not decoded, one
  without its header, one with the sidebands swapped).
- A session whose emission grows beyond its channel (an SSTV header tone
  becoming a picture) starts over on the whole of it.
- New: **time signal stations** are recognised by their second pulses
  (13 of the Signal Identification Wiki's 20 recordings of ten stations),
  and the **DCF77** and **MSF** time codes decoded: a minute is reported
  only when all its parities and fixed bits are right (both of the wiki's
  recordings decode).
- New: **WSPR** in its sub-bands, per 2-minute slot like FT8: callsign,
  locator and power, reported only when every field is valid and the
  message agrees with the tones received (WSPR has no checksum). Both of
  the Signal Identification Wiki's recordings decode; synthetic slots
  down to about -25 dB.
- New: **DRM** broadcasting is recognised by its OFDM guard interval and
  its robustness mode named (not decoded); narrower OFDM with DRM's timing
  is labelled "DRM-type OFDM" (WinDRM, EasyPal; others may share it). 3 of
  the Signal Identification Wiki's 4 DRM recordings, and its three
  WinDRM-type ones.

Frequency markers and databases
- Markers above the waterfall from frequency databases, as on KiwiSDR
  sites: click one and SDRconnect is set to it (demodulator, filter,
  frequency). A *Frequencies* window: the databases loaded, each on or
  off, only what is on the air now, a search over all of them, reading the
  databases again without a restart.
- EiBi's schedule is built in (winter 2026/27, 8971 entries, with hours
  on the air).
- New: **download** button in *Frequencies*: your PC fetches KiwiSDR's
  community label list (the military and utility markers of KiwiSDR
  sites) with its categories, and EiBi's current schedule, into the
  databases folder; nothing is shipped. Only the newest EiBi schedule in
  the folder is read.
- New: **my markers**: save a signal or a point with a label, a note and
  the mode; gold markers, listed in *Frequencies*, editable and deletable.
- New: **marker categories**: KiwiSDR labels carry their category
  (Milcom, Aero, HFDL, Marine, Time, ...), named and coloured by
  KiwiSDR's own category file once downloaded; tick categories on or off
  in *Frequencies*.
- New: a marker's details list **every station on that frequency** (within
  500 Hz) from all lists, on-air ones first, with their hours.
- Fixed: a KiwiSDR list with one VHF entry was read as Hz and lost its HF
  entries; the unit is now judged from the typical value.
- *Recording tasks*: which recordings would help the program learn, the
  frequencies the loaded databases list for each system, and recording
  from the window (SigMF) with a count per task.

Resources
- New: **Resources** window: processor time by activity and by signal
  every 2 s, a Stop button per signal, and controls that apply at once
  (and are saved): signals at once, leave a signal no decoder reads after
  N s, minimum SNR, measure signals no decoder fits or not, decoder
  families on and off.
- Changed: a signal no decoder fits is measured once and then left (it
  used to keep its channel and audio for as long as it lasted); when the
  sessions are full, a decodable signal takes the place of one that is
  only measured; undecided signals are left after 120 s by default.
- Fixed: a session whose emission's width wavered restarted over and over
  when decoders it would have had were switched off.

SDRconnect
- SDRconnect headless: its own output goes to `sdrconnect_headless.log`
  and its exit code to `hf_classifier.log`, with what SDRconnect reports
  on connecting. Connected but no IQ for 5 s with no radio open: the first
  radio is selected; still nothing: the window says so ("connected, but
  no signal arrives") instead of "connected".

Installer
- Fixed: the Bulgarian texts of the installer came out as garbage on an
  English Windows. The script is UTF-8 and had no byte order mark, so
  makensis read it in the system's ANSI code page (cp1252). It has the
  mark now, the build passes `-INPUTCHARSET UTF8`, and CI checks the mark.

## 0.3.0 (2026-11-14): first preview release

A preview, to try the program and see where it stands. **It has not yet
run against a real SDRconnect or RSPduo**: everything below is tested on
synthetic signals, on the Signal Identification Wiki's audio samples made
into IQ, and against a stand-in for SDRconnect (`testing/`).

**Try it without a receiver**: *HF Classifier demo* in the Start menu
(`hf_classifier --demo`) plays a synthetic band in a loop, with every
decoded mode and signals built like military systems, into its own log.

Decoding
- NAVTEX / SITOR-B, RTTY (45.45 to 100 Bd, 85 to 1000 Hz, at the measured
  rate and shift), CW, PSK31 to PSK500 in BPSK and QPSK, ALE 2G, 3G and 4G,
  FT8 and FT4.
- New: GMDSS digital selective calling (MF/HF DSC), reported only when a
  call's error-check character is right.
- 3G and 4G ALE bursts are now found when they are isolated (a session's
  decoders also hear the 0.75 s before it started); protected bursts are
  reported as "received intact, data encrypted".

Recognition by structure (nothing of the traffic is read or shown)
- New: Akula bursts, by their published sync and end groups.
- New: 7-bit constant-weight ("4:3") codes, as used by CIS-36-50 / CIS-50-50
  (T-600), SITOR and several ARQ systems.

Classification of what is not decoded
- Every emission is measured: modulation family, tones and shift, symbol
  rate, envelope, and (new) the period at which it repeats, which tells
  STANAG 4285, 4529 and 4539, MIL-STD-188-110 and CIS BPSK 1200 apart.
- The measured parameters are compared with a catalogue of 63 modes, with
  where each number comes from; the UI lists the modes an emission is
  *consistent with*, never an identification.
- New: a trained model gives its opinion of the family (FSK, MFSK, PSK,
  multi-carrier, OOK, sweep/pulse, analogue) with a confidence, or "not
  sure"; shown apart from the measurements.
- New: frequency lists (your own, a KiwiSDR's `dx.json`, OpenWebRX
  bookmarks, CSV) in the data folder's `frequencies` folder: an emission
  on a listed frequency shows the listing, and the decoder it names is
  tried.
- Better: FSK rate and shift on real recordings (the catalogue's right
  mode for RTTY on 14 of 19 wiki samples, from 9); weak wideband modems are
  one emission instead of fragments.

Program
- Windows installer (English and Bulgarian), the program in the
  notification area, settings dialog, automatic connection to SDRconnect
  and starting it without a window, web UI in Bulgarian and English, log
  in SQLite with search and CSV/JSON export, click-to-listen in SDRconnect.

On the wiki's samples (README, *sigidwiki_bench*): 55 of 82 samples of the
decoded or recognised modes; 5 labels on 992 samples of other systems,
four of them right and one undecided. For 115 of 223 samples of catalogued
modes a right mode is among those listed (59 on 2026-11-07).

## 0.2.0 (2026-10-21)

Emission detection, the decoder host with the decoders of the SDRuno
plugins, FT8/FT4, the program with its web UI and log. Not released.

## 0.1.0 (2026-10-19)

The SDRconnect connection: WebSocket client, IQ clock, recorder, probe.
Not released.
