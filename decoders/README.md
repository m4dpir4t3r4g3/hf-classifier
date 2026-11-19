# decoders: copies of your existing decoders

The decoders this program runs on emissions it can decode. They are copied,
unchanged, from the author's SDRuno plugin repositories; their documentation, design
notes and measured results stay there.

| Folder | From | Commit | Library | Decodes |
|---|---|---|---|---|
| `navtex/` | `SDR_plugin_NAVTEX` `core/` | dd57b4dded9f842afb7a4d119ce141d28e233221 | `navtex_core` | NAVTEX, SITOR-B, RTTY / any FSK, ALE 2G/3G/4G |
| `morse/` | `SDR_plugin_OOK` `core/` | 9a6fd13cfcbf2b6a65b69437856a550555d38830 | `morse_core` | Morse (CW), 5–50 WPM |
| `psk/` | `SDR_plugin_PSK` `dsp/` (without `AudioInput.h`, which is plugin glue) | 717a132e6994df3098911429d5de0b9ad960a1d3 | `pskdsp` | BPSK31, BPSK63, BPSK125, BPSK250, BPSK500; QPSK31, QPSK63, QPSK125, QPSK250, QPSK500 |

To update one, copy the folder again from a newer commit and change the
commit here; do not edit the copies. They all take real audio at any sample
rate; `core/src/decoder_host.cpp` feeds them USB audio made from SDRconnect's
IQ, one emission at a time.

Their own test suites stay in their repositories. Here they are tested
end to end on IQ: each decoder's own signal generator makes audio, the
audio is placed into IQ at an RF frequency, and the text has to come out of
this program (`tests/test_pipeline.cpp`).
