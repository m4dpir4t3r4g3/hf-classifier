# tools/ml: the learned signal-family model

The program's third layer of classification is a small trained model that
says which **family** a signal looks like, with a confidence, and says
nothing when it is not sure. It never names a system or a transmitter, and
its output is kept apart from what is measured.

| | |
|---|---|
| `tools/hfc_features` (C++) | the feature meter's vector, and a spectrogram, for audio files |
| `dataset.py` | builds a data set from the Signal Identification Wiki's samples |
| `train.py` | compares models, reports, exports the forest the program embeds |
| `catalogue/family_model.txt` | the exported model (text; format in `core/include/hfc/family_model.hpp`) |

```
cmake --build build --target hfc_features
tools/ml/dataset.py --features build/hfc_features --cache sigidwiki_cache --out family_data.jsonl
tools/ml/train.py --data family_data.jsonl --report report.md --export catalogue/family_model.txt
```

`dataset.py` needs ffmpeg and the cache that `tools/sigidwiki_bench.py`
downloads; `train.py` needs numpy and scikit-learn, and PyTorch for the
neural networks. The data set is not in the repository (the wiki's samples
are not ours to pass on); the exported model is, as numbers derived from
them.

## What the model sees

The same 86 numbers the C++ program computes for every emission
(`core/src/features.cpp`, the end of `FeatureMeter::measure`), so training
and use cannot drift apart:

- the averaged spectrum over 1.5 times the band, 32 bins, dB below its peak;
- where the instantaneous frequency dwells, 24 bins, weighted by power;
- the amplitude's distribution, 8 bins;
- the fourth-order cumulant C42; the strength of the strongest line of the
  squared and of the fourth-power signal (a carrier there: BPSK, QPSK);
- the meter's measurements: symbol-rate lines and their place in the band,
  envelope variation, time off, frequency levels and how tightly they hold
  the signal, tones, frame strength, bandwidth, spectral flatness.

Nothing in it depends on the signal's level. These are the classical
features of automatic modulation classification (cumulants, spectral lines
of powers of the signal, instantaneous-frequency statistics; Swami and
Sadler 2000, Dobre et al. 2007, as cited in `docs/RESEARCH.md` [32], from
memory: I did not re-read them, nor the surveys of 2021 to 2026).

## Labels, and what they are worth

A recording's label is the family its wiki page names in "modulation", when
it names one only: FSK, MFSK, PSK (single carrier, QAM too), multi-carrier
(OFDM), OOK, sweep/pulse (FMCW, pulsed), analogue (AM, FM, SSB). 903
recordings of 238 pages have one; 2197 clean 8 s windows.

The labels are noisy, and that bounds every score below: a page describes a
system, while a recording may show its idle tone or preamble; and the
tool measures the strongest band of the file, which is not always the
signal meant.

## Protocol

- **Pages are kept whole**: five folds over wiki pages, stratified by
  family; every window of every recording of a page is in one fold. A model
  is never scored on a system it has seen.
- Trained on the clean windows of four folds, scored on the fifth: clean,
  and with white noise added for 10 dB and 0 dB in 3 kHz. "+noise" trains
  on the noisy windows as well.
- Accuracy over windows and over recordings, and macro-averaged F1; mean
  and standard deviation over the folds.

## Results (2026-11-10)

| Model | Windows, clean | Recordings, clean | Macro F1 | Windows, 10 dB | Windows, 0 dB |
|---|---|---|---|---|---|
| rule-based meter (no model) | 44.2 ± 8.8 | - | 34.7 ± 5.8 | 41.3 ± 8.2 | 20.9 ± 3.6 |
| rule-based, on the five families it knows | 52.2 ± 8.9 | - | - | - | - |
| random forest, 300 trees | 60.1 ± 7.0 | 61.6 ± 7.2 | 50.2 ± 7.9 | 57.2 ± 4.5 | 30.7 ± 4.0 |
| random forest, 300 trees, + noise | 60.9 ± 6.9 | 62.0 ± 7.1 | 51.2 ± 6.0 | 63.6 ± 6.0 | 53.1 ± 3.3 |
| **random forest, 40 trees of depth 10, + noise (exported)** | 59.4 ± 7.2 | 60.8 ± 6.9 | 51.8 ± 6.2 | 62.1 ± 5.2 | 49.5 ± 3.8 |
| SVM (RBF) | 59.9 ± 6.6 | 60.7 ± 5.3 | 48.5 ± 5.7 | 54.1 ± 4.9 | 30.0 ± 1.5 |
| MLP (128, 64) | 58.8 ± 6.7 | 60.2 ± 7.0 | 48.7 ± 6.0 | 53.7 ± 5.9 | 31.8 ± 1.2 |
| CNN on the spectrogram (3 conv layers) | 40.6 ± 7.5 | 40.5 ± 7.5 | 31.5 ± 6.6 | 30.3 ± 7.3 | 16.3 ± 5.8 |
| ResNet on the spectrogram (3 residual blocks) | 49.9 ± 7.1 | 50.9 ± 7.9 | 40.2 ± 6.3 | 38.8 ± 8.3 | 21.3 ± 7.7 |
| Transformer over the spectrogram's time steps (2 layers) | 50.3 ± 4.9 | 51.8 ± 6.0 | 40.0 ± 4.2 | 35.5 ± 3.3 | 18.0 ± 2.4 |

The three neural networks see a 64 x 48 spectrogram of the same 8 s (log
power, 1.5 times the band), not the vector; 40 epochs, no tuning, no noise
in training.

What this says:

- On systems it has not seen, the model names the right one of seven
  families for about 60 % of signals; the rule-based meter 44 %. Chance,
  always answering FSK, is 28 %. The folds differ by 7 points: there are
  only 238 pages.
- The neural networks on spectrograms are 10 to 20 points behind the
  models on the vector, and fall apart in noise. With 2197 windows of 238
  pages that is what one expects, not a verdict on the architectures: a
  coarse spectrogram does not show a symbol rate or a carrier line, which
  the vector states outright, and there is far too little data to learn
  them. They took 34 minutes to train; the forest, seconds.
- The three classical models are within each other's spread. The forest is
  kept because it is the simplest to run without a library and gives a
  usable confidence.
- Training with noise costs nothing on clean signals and holds 50 % at
  0 dB, where the others fall to 30 %.

**Confidence** of the exported kind, on held-out pages (clean windows):

| Confidence at least | Share of the signals | Right among them |
|---|---|---|
| 0 (always answer) | 100 % | 59 % |
| 0.4 | 61 % | 78 % |
| **0.5 (the program's threshold)** | 44 % | 87 % |
| 0.6 | 33 % | 94 % |
| 0.7 | 26 % | 96 % |
| 0.8 | 18 % | 98 % |

So the program names a family for under half of the signals, and is then
right about seven times in eight; the rest get "not sure" with the closest
family and its percentage.

**Where it errs** (300-tree forest with noise, held-out pages; rows: the
page's family): multi-carrier is called PSK more often than multi-carrier
(86 against 74 of 207), OOK is mostly called something else (19 of 121
right), analogue likewise (33 of 136). FSK (463 of 618), MFSK (295 of 437),
PSK (319 of 466) and sweep/pulse (132 of 212) are the usable ones.

## Limits

- **The embedded model was trained on all the pages.** The scores above
  are of models of the same kind trained without the pages they are scored
  on; they estimate how the embedded one does on systems outside the wiki,
  and say nothing more.
- **One source of data.** Everything is from the wiki's clips: audio,
  mostly MP3, through someone's receiver. No IQ from an RSPduo, no
  recordings of yours, no synthetic signals yet.
- **In the whole program** the band comes from the detector, not from a
  file's spectrum as in training. Played through `hf_classifier`
  (`tools/sigidwiki_bench.py`, 2026-11-10), the strongest measured emission
  of 908 labelled samples got the right family from the embedded model for
  600 (66 %), and for 347 of the 385 it was sure of (90 %). These samples'
  pages were in its training, so this shows that the detector's bands do
  not upset it, not how it does on new systems (that is the table above).
  Decoding and labels in that run were the same as without the model.
- **RadioML is not used.** Its signals are simulated, 1024 samples at a few
  samples per symbol, of modulations rather than of HF systems; DeepSig
  itself warns of errors in the historical sets, and their licence is
  non-commercial share-alike. It would test a different thing.
- **"Not sure" is the only out-of-distribution guard**: a signal unlike
  anything in training usually splits the trees' votes, but nothing
  guarantees it.
- The labels are pages' words about systems (above).
