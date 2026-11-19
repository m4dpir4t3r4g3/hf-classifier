#!/usr/bin/env python3
"""Builds the data set for the learned family model from the Signal
Identification Wiki's samples (the cache of tools/sigidwiki_bench.py).

  tools/ml/dataset.py --features build/hfc_features --cache sigidwiki_cache --out family_data.jsonl

Each sample is cut to a minute, made 12 kHz mono, and measured by the
feature meter (hfc_features) in up to three 8 s windows: as it is, and with
white noise added for 10 dB and 0 dB in 3 kHz (to score the model against
noise it was not trained on). One line per window:

  {"entry": wiki page, "file": ..., "label": family or "", "snr": null|10|0,
   "rule": the rule-based meter's family, "vector": [...], "spectrogram": [...]}

The label is the family the wiki page's "modulation" field names, when it
names one only (pages naming several are kept unlabelled, for nothing):

  FSK            FSK, MSK, GFSK, FFSK
  MFSK           MFSK, IFK, 4FSK
  PSK            PSK, QAM (single carrier)
  multi-carrier  OFDM
  OOK            OOK, ASK, Morse
  sweep/pulse    FMCW, pulsed, FMOP
  analogue       AM, FM, SSB

These labels are what a page says about a system, not about each recording:
some recordings show another state of the system (an idle tone, a preamble),
and the tool measures the strongest band of the file, which is not always
the signal meant. The wiki's samples are not ours to redistribute; nor is
this file. Needs ffmpeg.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import tempfile


def family(modulation):
    m = modulation.upper().replace("‑", "-")
    found = set()
    if re.search(r"FMCW|PULSE|FMOP|CHIRP", m):
        found.add("sweep/pulse")
    m = m.replace("FMCW", "")
    if "OFDM" in m:
        found.add("multi-carrier")
    if re.search(r"MFSK|IFK|4FSK|8FSK", m):
        found.add("MFSK")
    if re.search(r"(?<![M48])FSK|MSK", m):
        found.add("FSK")
    if ("PSK" in m or "QAM" in m) and "OFDM" not in m:
        found.add("PSK")
    if re.search(r"OOK|ASK|\bCW\b", m):
        found.add("OOK")
    if re.search(r"\bAM\b|\bFM\b|\bUSB\b|\bLSB\b|\bSSB\b|\bNFM\b", m):
        found.add("analogue")
    return next(iter(found)) if len(found) == 1 else ""


def measure(job):
    features, cache, entry, f, label = job
    ext = f["name"].rsplit(".", 1)[-1].lower()
    src = os.path.join(cache, "audio", hashlib.md5(f["name"].encode()).hexdigest() + "." + ext)
    if not os.path.exists(src):
        return []
    out = []
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "a.f32")
        subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-t", "60", "-ac", "1", "-ar", "12000", "-f", "f32le", raw],
                       stderr=subprocess.DEVNULL)
        if not os.path.exists(raw):
            return []
        for snr in (None, 10, 0):
            cmd = [features, raw, "--windows", "3", "--spectrogram"] + (["--snr", str(snr)] if snr is not None else [])
            for line in subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines():
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                out.append({"entry": entry, "file": f["name"], "label": label, "snr": snr, "rule": r["modulation"],
                            "summary": r["summary"], "vector": r["vector"], "spectrogram": r["spectrogram"]})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--features", required=True, help="path to hfc_features")
    ap.add_argument("--cache", default="sigidwiki_cache")
    ap.add_argument("--out", default="family_data.jsonl")
    ap.add_argument("--bands", default="HF,MF,LF,VLF")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    entries = json.load(open(os.path.join(a.cache, "entries.json"), encoding="utf-8"))
    bands = set(a.bands.split(","))
    jobs = [(a.features, a.cache, t, f, family(e.get("modulation", "")))
            for t, e in sorted(entries.items()) if bands & set(e["categories"]) for f in e["files"]]
    n = 0
    with open(a.out, "w", encoding="utf-8") as out, concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for rows in ex.map(measure, jobs):
            for r in rows:
                out.write(json.dumps(r, ensure_ascii=False) + "\n")
                n += 1
    print("%d samples, %d windows written to %s" % (len(jobs), n, a.out))


if __name__ == "__main__":
    main()
