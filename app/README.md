# app: hf_classifier and HFClassifier

The program: SDRconnect's IQ (or a SigMF recording) in; emissions,
decodes, a log and a web UI out.

| File | What it is |
|---|---|
| `classifier_app` | One `Pipeline` per receiver in its own thread, the SQLite log, the web server and its API |
| `sdrconnect_source` | The live source: connection, set-up, reconnection, starting `SDRconnect_headless`, the connection state shown in the UI, the UI's `listen` and `tune` commands |
| `runner` | Options and settings, single instance, the live loop (restarted when the source settings change), playback of SigMF recordings, `--report` |
| `diagnostics` | The diagnostic report: one ZIP with the program's and Windows' versions, the settings, the connection state, SDRconnect's install, version and port, the probe's measurements, and the logs |
| `settings` | `settings.json` in the data folder: reading, checking, writing |
| `platform` | What comes from the operating system: finding SDRconnect, whether it runs, starting and stopping a program, opening the window, the system language |
| `main.cpp` | `hf_classifier.exe`: the program in a console |
| `tray_main.cpp` | `HFClassifier.exe` (Windows): the program without a console, in the notification area |
| `ui/index.html` | The whole UI (HTML, CSS, JavaScript, Bulgarian and English, no external files), embedded into the program at build time by `cmake/embed.cmake`, with `resources/hf_classifier.png` as its icon |

## Data folder and settings

`%LOCALAPPDATA%\HF Classifier` on Windows (`$XDG_DATA_HOME/hf_classifier`
or `~/.local/share/hf_classifier` elsewhere) holds `settings.json`, the
log `hf_classifier.db`, and for `HFClassifier.exe` its messages,
`hf_classifier.log` (new each run; the run before's is kept as
`hf_classifier.previous.log`, and a second start that only opens the
running one's window leaves both alone), and diagnostic reports in
`reports`. Paths may contain any characters
(Cyrillic user names): files are opened through Windows' wide-character
functions.

| Key | Default | |
|---|---|---|
| `language` | `""` | `bg`, `en`, or `""` for the system's display language |
| `sdr_host`, `sdr_port` | `127.0.0.1`, 5454 | SDRconnect's WebSocket server |
| `tuner` | `1` | `1`, `2` or `both` |
| `dual_serial` | `""` | switch this RSPduo to dual tuner mode first |
| `start_headless`, `headless_path` | true, `""` | start `SDRconnect_headless` when nothing answers; `""` = find it |
| `http_port` | 8073 | the window's port |
| `open_window` | true | open the window at start |
| `ft8` | true | decode the FT8/FT4 sub-bands |
| `max_sessions` | 48 | emissions decoded at once |

A missing file means the defaults; a file that is not JSON is reported and
the defaults are used. The file is written to `settings.json.new` and then
renamed over the old one. The Settings dialog saves only what it changed,
so the file never takes up a run's command-line options. Changing the
SDRconnect settings reconnects at once; `ft8`, `http_port` and
`max_sessions` take effect at the next start.

## Connection states

What the live source is doing, shown in the status chip and, when something
needs the user, in a banner with steps and buttons:

| State | When |
|---|---|
| `connecting` | trying to connect |
| `connected`, `view_only` | connected; `view_only` when SDRconnect does not accept control |
| `starting_headless` | `SDRconnect_headless` was started, waiting up to 20 s for it to answer |
| `headless_failed` | it did not answer, or could not be started |
| `websocket_off` | the SDRconnect window runs (process `SDRconnect.exe`) but nothing answers: its WebSocket server is off |
| `not_installed` | nothing answers, and no `SDRconnect_headless.exe` was found |
| `no_answer` | nothing answers (another computer, the headless start turned off, or not Windows) |
| `playback`, `playback_done`, `playback_failed` | playing a recording |

The headless program is started only for an address on this computer,
when the SDRconnect window is not running, at most once a minute, hidden,
with `--websocket_port=N`; it is stopped when the program exits or the
settings change. Where SDRconnect is installed is looked up in
`Program Files\SDRplay\SDRconnect` and in Windows' list of installed
programs (`InstallLocation` of an entry named "SDRconnect…").
**Unverified:** the process name, the folder, and the option, until tried
with a real installation. The program does not notice the SDRconnect window
being opened while the headless SDRconnect it started runs; both would want
the receiver, so exit HF Classifier first (or turn the headless start off).

## HFClassifier.exe

No console. A notification-area icon: click to open the window, right-click
for a menu (open the window, open the data folder, exit). Without the window
opened at start, a balloon says where the program went. Start-up problems
(the port in use, bad options) are shown in a message box. A second start
opens the first one's window and exits: the first answers `GET /api/status`
on the port. Windows shutting down, *Exit*, or `WM_CLOSE` to its window (the
installer and uninstaller send that) stop it cleanly: the headless SDRconnect
it started is stopped and the log closed. The window is an app window of
Edge or Chrome (`--app=URL`, found through `App Paths` in the registry),
else the default browser.

## Threads

```
SDRconnect reader thread (Client) --IqInput--> queue (<= 5 s) --> receiver thread: Pipeline
                                                                   | emissions, decodes -> Store (SQLite), browsers
                                                                   ` waterfall rows     -> browsers (dropped for slow ones)
FT8/FT4 worker thread (DecoderHost) -> results picked up by the receiver thread
web server: accept thread, one thread per connection, one sender thread per WebSocket client
```

The queue keeps the WebSocket reader fast, which matters because SDRconnect drops
IQ when a reader falls about 2.5 s behind. When processing cannot keep up,
blocks beyond 5 s of queue are dropped and counted. The UI shows the count.
IQ is processed at the sample rate SDRconnect reports; blocks that arrive
before it has reported one, and the 0.1 s after a retune, are skipped.

A browser that falls behind loses waterfall rows; one that stops reading
altogether (4096 messages queued) is disconnected. A browser that connects
gets what is on the air and this run's last 200 decodes, then the changes;
on reconnecting it starts afresh.

## Web interface

`http://127.0.0.1:8073/`, listening on 127.0.0.1 only. Requests whose
`Host` is not this machine, and WebSockets opened from another site's page
(`Origin`), are refused. This keeps other web pages from tuning your receiver.

| Path | |
|---|---|
| `/` | the UI |
| `/ws` | WebSocket: live status, emissions, decodes (JSON text), waterfall rows (binary), commands |
| `/api/emissions`, `/api/decodes` | the log as JSON. Filters: `from`, `to` (Unix seconds or ISO UTC), `min_khz`, `max_khz`, `label` (decoder name, or `-` for undecoded), `text`, `limit`. `/api/decodes?session=S&rx=R&emission=E` gives one emission's text (each receiver numbers its emissions from 1) |
| `/export.csv`, `/export.json` | the same filters; one row per emission with its decoded text (CSV), or emissions with their decodes (JSON) |
| `/api/status` | mode, connection state, receivers, rates, centres, dropped samples, settings, system language, log path |
| `/api/markers` | entries of the frequency databases: `lo_khz`, `hi_khz` (a band), `text` (a search), `limit`. Each has its frequency, label, catalogued mode, source, how to listen (`demod`), whether it is on the air now, and its hours if the list gives them |
| `/api/databases` | the databases loaded with their entry counts, and their folder |
| `/api/tasks` | the recording tasks (`catalogue/recording_tasks.json`), each with the frequencies the databases list for it and how many recordings there are |
| `/api/my_markers` | the user's markers (`my_markers.json` in the frequency folder): id, frequency, label, note, mode, when added |
| `/api/usage` | where each receiver's processing went in the last 2 s (the last `usage` message of each) |
| `/reports/NAME.zip` | a diagnostic report from the reports folder, as a download |
| `/api/windows` | the user's frequency windows (`decode_windows.json` in the frequency folder: id, `lo_hz`, `hi_hz`, `decoder` or `none`), the decoders a window or marker can be given, and whether they can be saved |
| `/icon.png` | the program's icon |

Commands on the WebSocket beyond `listen` and `tune`: `record` (`rx`,
`task`, `freq`, `seconds`; or `stop`), which writes the receiver's IQ as
SigMF into the recordings folder and reports with `recording` messages;
`reload_databases`; `download_databases` (fetches the lists into the
frequency folder in the background, reporting with `download` messages:
each list's name, file, ok, entries, error); `marker_save` (`hz`,
`label`, `note`, `demod`, and `id` to change one) and `marker_delete`
(`id`), after which a `my_markers` message goes out; `ignore` (`rx`, `id`: leave that emission for good);
`open_folder` (`which`: `frequencies`, `recordings` or `reports`);
`window_save` (`lo_hz`, `hi_hz`, `decoder`: one of `/api/windows`'
decoders, `RTTY <baud>/<shift>`, or `none`; `id` to change one) and
`window_delete` (`id`), after which a `windows` message goes out (the
receivers get the new rules from their next block: windows, and markers
with a `decoder`, which `marker_save` now takes, as ±1.5 kHz around them);
`diagnostics` (`rates`: `1` to measure each sample rate too), which makes a
diagnostic report in a thread of its own and reports with `diagnostics`
messages (`state`: `running`, `line` with a line of the probe's progress,
`done` with `file` and `path`, `failed` with `message`). Live, the probe
runs on the source's own connection: the source reports the state
`measuring`, IQ goes to the probe instead of the receivers, and the
connection is set up again afterwards (`SdrconnectSource::probe`). The
status says whether reports can be made (`diagnostics`). `listen` with `dial: 1` sets the VFO to `freq` itself (a
list's entry) instead of placing `freq` in the middle of the passband.

WebSocket messages:

- Server to browser: `{"type":"status",...}`, `{"type":"emission","event":"start"|"update"|"end",...}`, `{"type":"decode",...}`, `{"type":"result",...}`.
  An emission's measured features (core/README.md, *Features of every emission*) come in an `update` once measured,
  as `"features":{"modulation":"FSK","tones":2,"spacing_hz":850,"symbol_rate":75,...,"summary":"2-FSK, shift 850 Hz, 75.0 Bd",`
  `"consistent":[{"mode":"RTTY (other rates and shifts)","provenance":"observed","partial":false},...]}` (the catalogue modes they
  are consistent with, best first);
  `/api/emissions` and the JSON export carry them too, the CSV export their summary.
- Browser to server: `{"cmd":"listen","rx":0,"freq":HZ,"mode":"USB"|"LSB"|"CW"|"AM","bw":HZ}`, `{"cmd":"tune","rx":0,"centre":HZ}`,
  `{"cmd":"settings", key: value...}` (any of the settings keys above) and `{"cmd":"reconnect"}`.
- A `result` that failed carries a code (`not_connected`, `not_controllable`, `bad_port`, `bad_host`, `save_failed`, ...)
  that the UI puts into words in its language.

A waterfall row is binary: `WF01`, the receiver (u8), the number of bins
(u16), then UTC, low and high edge in Hz (f64), all little-endian. Then one
byte per bin: 40 + 4 × (dB above the row's median noise floor).

## Click to listen

`listen` sets SDRconnect's demodulator, filter bandwidth and VFO so that the
emission sits in the middle of the passband. In USB the VFO is set to centre − bandwidth/2;
in LSB to centre + bandwidth/2; in CW and AM to the centre. **Unverified:** this
assumes SDRconnect's `device_vfo_frequency` is the suppressed-carrier (dial)
frequency in USB/LSB, the usual convention. The API document says only
"VFO tuned frequency"; third-party programs that set the VFO to the FT8 dial
frequency in USB and decode SDRconnect's audio support it. SDRconnect also
has an `ssb_lowcut_frequency` that this program does not set: if the SSB
passband runs from VFO + lowcut, emissions sit lowcut/2 off-centre (some
100 Hz; still audible). If emissions sound off-centre, tell me what you hear. `tune` sets
`device_center_frequency`; the classifier follows the new centre from the
IQ, discarding the 0.1 s after a retune, which may still hold the old frequency.

Both need SDRconnect to accept control (`can_control`); otherwise the UI
says so. In playback they are not available.

## Limitations

- Not yet run against a real SDRconnect: tested against the fake
  SDRconnect (one tuner, both tuners of a dual-tuner RSPduo, following a
  retune, listen and tune commands) and on synthetic recordings.
- One processing thread per receiver for the spectrum and the detector;
  the decoding sessions run on a pool of threads besides it (as many as the
  processors less one, at most 16 per receiver). Measured speed of the
  spectrum and detector alone, on one core of the development machine (a
  cloud VM), with the band empty of signals:

  | Sample rate | Faster than real time by |
  |---|---|
  | 2 MS/s | 8.9× |
  | 6 MS/s | 2.9× |
  | 10 MS/s | 1.7× |

  On a busy band the decoders cost more than the spectrum. On
  `make_busy_scene`'s synthetic band (2.048 MS/s, about 120 signals, 30 s)
  the development machine (4 processors) takes 17 s; with one thread, as
  0.4.0 did, 37 s, which is slower than real time. When the processing
  falls behind, IQ is dropped and the window says so (each `usage` message
  carries `dropped`, the seconds dropped so far). At 10 MS/s the FFT (KISS
  FFT, 320000 points), which runs in one thread, takes most of the time.
- The log grows without limit. It is an ordinary SQLite file: delete it, or
  pass `--db` to start another.
- The browser keeps the newest 2000 emissions. *Search log* queries the whole database.

Every 2 s, while IQ arrives, each receiver sends a `usage` message: `cpu`
(% of one processor core over the interval, all threads together, so it
can pass 100 %), `busy` (% of the interval the receiver's thread was
processing: at 100 % it cannot keep up; the UI's load chip shows it),
`dropped` (seconds of IQ dropped so far), `left_ids` (emissions still
known that were left: `g` given up, `i` stopped, `m` measured only),
`load` (processing time over IQ time, %), `items` (by activity: "spectrum and detector",
"decoder CW", "measuring signals", "FT8/FT4 sub-bands"...), `sessions`
(each: emission, centre, width, SNR, `state` decoding / trying / measuring,
decoders, age, `pct`), `counts` and `left` (emissions given up, ignored,
measured with nothing to decode them). The times are measured with the
steady clock around each piece of work in the receiver's thread and the
FT8/WSPR worker; the spectrum's share is the rest of the receiver thread's
time.
