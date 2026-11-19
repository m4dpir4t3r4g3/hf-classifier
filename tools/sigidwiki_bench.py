#!/usr/bin/env python3
"""Plays the audio samples of the Signal Identification Wiki
(https://www.sigidwiki.com, the source of Artemis's database) through
hf_classifier and reports, per wiki entry, what was decoded.

The samples are USB audio (mostly MP3), not IQ: each one is made into IQ
(the analytic signal at 62.5 kS/s, with white noise at the level of the
quietest part of the receiver's passband added across the whole band)
and played with --speed 0. That is close to, but not the same as, IQ from
an RSPduo: short, mostly strong and clean clips, compressed, through
someone's receiver and sound card.

Each sample is played twice, with the added noise at the passband's typical
level and at its quietest part (to_iq); what either run finds counts. That
also gives false labels two chances.

Needs Python 3.8+, numpy, ffmpeg on the PATH, and network access to
sigidwiki.com the first time (pages and samples are kept in --cache).

  tools/sigidwiki_bench.py --classifier build/hf_classifier [--cache DIR]
      [--entries REGEX] [--bands HF,MF,LF,VLF] [--jobs 8] [--report out.md]

The expected labels per entry are in EXPECTED below. Every label on any
other entry is listed as "to review": either a false label, or a signal of
a decoded mode inside the recording (an ALE call in a modem recording).
Samples longer than 3 minutes are cut. The wiki's samples are not ours to
redistribute: they stay in the cache.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import urllib.parse

import numpy as np

API = "https://www.sigidwiki.com/api.php"
AGENT = "hf-classifier-benchmark/1.0"
RATE = 62500

# Wiki entry -> labels that are correct for it (prefixes).
EXPECTED = {
    "Morse Code (CW)": ["CW"],
    "Coherent CW": ["CW"],
    "Radio Teletype (RTTY)": ["RTTY"],
    "SITOR-B": ["SITOR-B", "NAVTEX"],
    "GMDSS Digital Selective Calling": ["DSC"],
    # Systems with 7-bit constant-weight (4:3) codes: the code is recognised, not the system.
    "CIS-36-50": ["4:3 code"],
    "CIS-50-50": ["4:3 code"],
    "POL-ARQ": ["4:3 code"],
    "SWED-ARQ": ["4:3 code"],
    "ARQ-E(E3)": ["4:3 code"],
    "ARQ-N": ["4:3 code"],
    "Phase Shift Keying (PSK)": ["PSK", "QPSK"],
    "Automatic Link Establishment (2G ALE)": ["ALE 2G"],
    "Automatic Link Establishment (3G ALE ARCS)": ["ALE 3G"],
    "Automatic Link Establishment (4G ALE WALE)": ["ALE 4G"],
    "FT8": ["FT8"],
    "FT4": ["FT4"],
    "High Frequency Data Link (HFDL)": ["HFDL"],
    "WEFAX": ["FAX"],
    "Slow-Scan Television (SSTV)": ["SSTV"],
    "WSPR": ["WSPR"],
    # DRM is recognised (its OFDM guard interval), not decoded; WinDRM and
    # EasyPal use the same modes.
    "Digital Radio Mondiale (DRM)": ["DRM"], "WinDRM": ["DRM"], "EasyPal Digital SSTV": ["DRM"],
    "UK GOV MIL WINDRM51": ["DRM"],
    # Time signals: the seconds are recognised (the label), DCF77 and MSF decoded.
    "DCF77": ["TIME"], "MSF": ["TIME"], "RBU": ["TIME"], "RWM": ["TIME"], "BPM": ["TIME"], "CHU": ["TIME"],
    "TDF": ["TIME"], "JJY": ["TIME"], "WWVB": ["TIME"], "UTC (Coordinated Universal Time) Time Standard": ["TIME"],
}
# Wiki entry -> catalogue modes (catalogue/modes.json) that are right for it,
# for scoring the "consistent with" lists of the catalogue matcher.
CATALOGUE_OF = {
    "SITOR-A": ["SITOR-A"],
    "SITOR-B": ["NAVTEX / SITOR-B"],
    "GMDSS Digital Selective Calling": ["DSC-HF"],
    "STANAG 4285": ["STANAG 4285"],
    "STANAG 4529": ["STANAG 4529"],
    "STANAG 4539": ["STANAG 4539 / MIL-STD-188-110 App C"],
    "STANAG 4415": ["MIL-STD-188-110 serial / STANAG 4415"],
    "MIL-STD-188-110 Serial": ["MIL-STD-188-110 serial / STANAG 4415"],
    "Israeli Navy Hybrid Modem (188-110 MOD)": ["MIL-STD-188-110 serial / STANAG 4415"],  # 110A-MOD, a variant
    "MIL-STD-188-110 Appendix A 16-Tone": ["MIL-STD-188-110 App A (16-tone)"],
    "MIL-STD-188-110 Appendix B 39-Tone": ["MIL-STD-188-110 App B (39-tone) / STANAG 4197"],
    "STANAG 4197": ["MIL-STD-188-110 App B (39-tone) / STANAG 4197"],
    "High Frequency Data Link (HFDL)": ["HFDL"],
    "Link-11": ["Link-11 CLEW", "Link-11 SLEW"],
    "CIS-12": ["CIS-12"],
    "CIS-36-50": ["CIS-36-50 (T-600)"],
    "CIS-50-50": ["CIS-50-50"],
    "CIS-1200 (T-230-1A 'Mahovik')": ["CIS BPSK 1200"],
    "CIS MFSK-16 XPA2": ["CIS MFSK-16 (XPA2)"],
    "CIS-112": ["CIS-112"],
    "CIS-3000": ["CIS-3000"],
    "CIS-20": ["CIS-20"],
    "Serdolik": ["Serdolik (command state)", "CROWD36 (CIS-36 MFSK)"],  # the wiki page mixes the two
    "PACTOR I": ["PACTOR I"],
    "PACTOR II": ["PACTOR II"],
    "PACTOR III": ["PACTOR III"],
    "Polish Intelligence 100bd 625Hz FSK": ["Polish F11"],
    "Chinese 4+4": ["Chinese 4+4"],
    "Codan Data Modem": ["CODAN 3212", "CODAN chirp (ALE)", "CODAN 9001", "CODAN Selcal"],
    "Automatic Link Establishment (2G ALE)": ["ALE 2G"],
    "Automatic Link Establishment (3G ALE ARCS)": ["ALE 3G / 4G (WALE)"],
    "Automatic Link Establishment (4G ALE WALE)": ["ALE 3G / 4G (WALE)"],
    "Olivia": ["Olivia"],
    "Multi Frequency Shift Keying (MFSK)": ["MFSK16", "MFSK32"],
    "WSPR": ["WSPR"],
    # DRM is recognised (its OFDM guard interval), not decoded; WinDRM and
    # EasyPal use the same modes.
    "Digital Radio Mondiale (DRM)": ["DRM"], "WinDRM": ["DRM"], "EasyPal Digital SSTV": ["DRM"],
    "UK GOV MIL WINDRM51": ["DRM"],
    "Phase Shift Keying (PSK)": ["PSK31", "PSK63", "PSK125", "PSK250", "PSK500",
                                 "QPSK31", "QPSK63", "QPSK125", "QPSK250", "QPSK500"],
    "Radio Teletype (RTTY)": ["RTTY 45.45/170", "RTTY 50/450", "RTTY (other rates and shifts)"],
    "Morse Code (CW)": ["CW"],
    "Hellschreiber": ["Feld Hell"],
}
# FT8/FT4 are decoded per sub-band at their dial frequencies, per UTC slot:
# these samples are put at a dial frequency and tried at several slot phases.
FT_DIALS = {"FT8": (14074000, [0, 2, 4, 6, 8, 10, 12, 14]), "FT4": (14080000, [0, 1, 2, 3, 4, 5, 6, 7]),
            # WSPR: 2-minute slots; offsets from an even minute.
            "WSPR": (14095600, [0, -1, 1, 2])}
OTHER_DIAL = 7000000  # no FT8/FT4 sub-band within 31 kHz
# Entries played as on a frequency a list names for them: HFDL channels are
# listed (EiBi has 124), and the program decodes a listed one all the time.
LISTED = {"High Frequency Data Link (HFDL)": "HFDL"}


def api(params):
    params = dict(params, format="json")
    out = subprocess.run(["curl", "-s", "-f", "-A", AGENT, "--data", urllib.parse.urlencode(params), API],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def fetch_entries(cache):
    path = os.path.join(cache, "entries.json")
    if os.path.exists(path):
        return json.load(open(path, encoding="utf-8"))
    titles = set()
    for direction in ("asc", "desc"):  # the server lists at most 500 and gives no continuation
        d = api({"action": "query", "list": "categorymembers", "cmtitle": "Category:Signal", "cmlimit": "500",
                 "cmdir": direction})
        titles |= {m["title"] for m in d["query"]["categorymembers"]}
    titles = sorted(titles)
    entries = {}
    for i in range(0, len(titles), 50):
        d = api({"action": "query", "titles": "|".join(titles[i:i + 50]), "prop": "revisions", "rvprop": "content"})
        for pg in d["query"]["pages"].values():
            revs = pg.get("revisions", [])
            text = revs[0].get("*", "") if revs else ""
            fields = {}
            m = re.search(r"\{\{Signal(.*?)\n\}\}", text, re.S)
            if m:
                for line in m.group(1).split("\n|"):
                    if "=" in line:
                        k, v = line.split("=", 1)
                        fields[k.strip().lstrip("|")] = v.strip()
            names = set()
            if fields.get("Signal file"):
                names.add(fields["Signal file"])
            names |= {n.strip() for n in re.findall(r"(?:File|Media):([^|\]}\n]+?\.(?:mp3|ogg|oga|wav|flac))", text, re.I)}
            entries[pg["title"]] = {
                "categories": [c.strip() for c in fields.get("Additional categories", "").split(",") if c.strip()],
                "modulation": fields.get("Modulation", ""),
                "files": sorted(n for n in names if n),
            }
    names = sorted({n for e in entries.values() for n in e["files"]})
    urls = {}
    for i in range(0, len(names), 50):
        d = api({"action": "query", "titles": "|".join("File:" + n for n in names[i:i + 50]), "prop": "imageinfo",
                 "iiprop": "url|size"})
        norm = {n["to"]: n["from"] for n in d["query"].get("normalized", [])}
        for pg in d["query"]["pages"].values():
            if pg.get("imageinfo"):
                urls[norm.get(pg["title"], pg["title"])[5:]] = pg["imageinfo"][0]["url"]
    for e in entries.values():
        e["files"] = [{"name": n, "url": urls[n]} for n in e["files"] if n in urls]
    json.dump(entries, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    return entries


def local_file(cache, f):
    ext = f["name"].rsplit(".", 1)[-1].lower()
    path = os.path.join(cache, "audio", hashlib.md5(f["name"].encode()).hexdigest() + "." + ext)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        r = subprocess.run(["curl", "-s", "-f", "-A", AGENT, "-o", path + ".part", f["url"]])
        if r.returncode != 0:
            return None
        os.replace(path + ".part", path)
    return path


def to_iq(audio_path, seconds=180, noise="quiet"):
    """USB audio -> complex IQ at RATE (audio 0 Hz at the centre), or None if
    undecodable. White noise is added across the whole band, so that the
    band outside the audio is not digital silence: at the passband's typical
    noise level ("passband": a CW filter's passband, MP3 hiss, would
    otherwise look like one wide emission), or at the level of its quietest
    part ("quiet": a modem that is on all the time would otherwise be buried
    in noise at its own level). Which is right cannot be told from the audio
    alone, so each sample is played with both."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", audio_path, "-t", str(seconds), "-ac", "1", "-ar", str(RATE),
                          "-f", "f32le", "-"], capture_output=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).copy()
    if x.size < RATE:
        return None
    x -= x.mean()
    n = x.size
    X = np.fft.rfft(x)
    full = np.zeros(n, dtype=np.complex64)  # analytic signal: positive frequencies only
    full[:X.size] = X
    full[1:X.size] *= 2
    if n % 2 == 0:
        full[X.size - 1] /= 2
    del X
    z = np.fft.ifft(full).astype(np.complex64)
    del full
    # Noise per frequency: the 20th percentile over time of |FFT|^2 (Hann,
    # 4096 samples), so that intermittent signals drop out. The passband's
    # noise: the quietest tenth of the receiver's passband over 200..3000 Hz,
    # the passband being where that floor is within 25 dB of its 90th
    # percentile (so that a CW filter's stopband does not count, and a modem
    # that is on all the time but leaves some of the passband free does not
    # pass for noise). Noise power is exponentially distributed: mean = 20th
    # percentile / 0.223.
    L = 4096
    segs = z[: (n // L) * L].reshape(-1, L)[:400]
    fb = np.fft.fftfreq(L, 1 / RATE)
    floor = np.percentile(np.abs(np.fft.fft(segs * np.hanning(L), axis=1)) ** 2, 20, axis=0)
    band = floor[(fb > 200) & (fb < 3000)]
    if noise == "passband":  # the passband's typical noise (a CW filter's passband, MP3 hiss)
        level = np.percentile(band, 80) / 0.223
    else:  # its quietest part (a modem that is on all the time)
        passband = band[band >= np.percentile(band, 90) / 10 ** 2.5]
        level = np.percentile(passband, 10) / 0.223
    sigma2 = level / (L * 0.375)  # a Hann window's sum of squares is 0.375 L
    rng = np.random.default_rng(1)
    z += (np.sqrt(sigma2 / 2) * (rng.standard_normal(n, dtype=np.float32) + 1j * rng.standard_normal(n, dtype=np.float32))).astype(np.complex64)
    z *= 0.5 * 32767 / max(1e-12, np.abs(z).max())
    iq = np.empty(2 * n, dtype=np.int16)
    iq[0::2] = np.round(z.real)
    iq[1::2] = np.round(z.imag)
    return iq


def features_summary(text):
    try:
        return json.loads(text).get("summary", "") if text else ""
    except ValueError:
        return ""


def features_consistent(text):
    try:
        return [c["mode"] for c in json.loads(text).get("consistent", [])] if text else []
    except (ValueError, KeyError, TypeError):
        return []


def features_predicted(text):
    """The learned model's family, confidence and whether it is sure ([] if none)."""
    try:
        p = json.loads(text).get("predicted") if text else None
        return [p["family"], p["confidence"], bool(p["sure"])] if p else []
    except (ValueError, KeyError, TypeError, AttributeError):
        return []


def features_frame(text):
    """The consistent modes whose catalogued frame period agreed."""
    try:
        return [c["mode"] for c in json.loads(text).get("consistent", []) if c.get("frame")] if text else []
    except (ValueError, KeyError, TypeError, AttributeError):
        return []


def play(classifier, iq, dial, offset, listed=""):
    with tempfile.TemporaryDirectory() as d:
        freq = ""
        if listed:
            freq = os.path.join(d, "freq")
            os.mkdir(freq)
            open(os.path.join(freq, "list.txt"), "w").write("%.1f %s\n" % (dial / 1000.0, listed))
        iq.tofile(os.path.join(d, "s.sigmf-data"))
        sec = 1700000000 - 1700000000 % 120 + offset  # an even minute (every slot's boundary) + offset
        dt = datetime.datetime.fromtimestamp(sec, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        json.dump({"global": {"core:datatype": "ci16_le", "core:sample_rate": RATE, "core:version": "1.2.0"},
                   "captures": [{"core:sample_start": 0, "core:frequency": dial, "core:datetime": dt}],
                   "annotations": []}, open(os.path.join(d, "s.sigmf-meta"), "w"))
        open(os.path.join(d, "settings.json"), "w").write("{}")
        p = subprocess.run([classifier, "--settings", os.path.join(d, "settings.json"), "--play",
                            os.path.join(d, "s.sigmf-meta"), "--speed", "0", "--exit", "--db", os.path.join(d, "x.db"),
                            "--http", "0", "--no-open",
                            "--frequencies", freq], capture_output=True, text=True, timeout=1800)
        if p.returncode != 0:
            return {"error": "hf_classifier: %d %s" % (p.returncode, p.stderr[-200:])}
        c = sqlite3.connect(os.path.join(d, "x.db"))
        res = {
            "emissions": c.execute("select count(*) from emissions").fetchone()[0],
            "labels": dict(c.execute("select label, count(*) from emissions where length(label) > 0 group by label")),
            # The strongest emissions, relative to the dial: low and high edge (Hz), mean SNR (dB), seconds,
            # label, measured features, consistent modes, those of them whose frame period agreed.
            "strongest": [list(r[:5]) + [features_summary(r[5]), features_consistent(r[5]), features_frame(r[5]),
                                         features_predicted(r[5])]
                          for r in c.execute(
                "select round(lo_hz - ?), round(hi_hz - ?), round(mean_snr_db, 1), round(end_utc - start_utc, 1), label, "
                "features from emissions order by (end_utc - start_utc) * mean_snr_db desc limit 3", (dial, dial))],
            "decodes": [list(r) for r in c.execute("select decoder, round(freq_hz - ?), text from decodes order by id limit 30",
                                                   (dial,))],
        }
        c.close()
        return res


def run_sample(classifier, cache, title, f, noises=("passband", "quiet")):
    """Plays one sample with each noise level (to_iq); labels, decodes and
    the strongest emissions of all the runs count."""
    path = local_file(cache, f)
    if not path:
        return {"title": title, "file": f["name"], "error": "download failed"}
    out = {"title": title, "file": f["name"], "labels": {}, "decodes": [], "emissions": 0, "strongest": []}
    for noise in noises:
        iq = to_iq(path, noise=noise)
        if iq is None:
            return {"title": title, "file": f["name"], "error": "too short or undecodable"}
        out["seconds"] = round(iq.size / 2 / RATE, 1)
        runs = [(OTHER_DIAL, 0)]
        if title in FT_DIALS:
            dial, offsets = FT_DIALS[title]
            runs += [(dial, o) for o in offsets]
        first = True
        for dial, offset in runs:
            r = play(classifier, iq, dial, offset, LISTED.get(title, ""))
            if "error" in r:
                out["error"] = r["error"]
                return out
            out["emissions"] = max(out["emissions"], r["emissions"])
            if first:
                out["strongest"] += r["strongest"]
                out.setdefault("strongest_by_noise", {})[noise] = r["strongest"]
                first = False
            for k, v in r["labels"].items():
                out["labels"][k] = max(out["labels"].get(k, 0), v)
            if r["decodes"] and len(r["decodes"]) > len(out["decodes"]):
                out["decodes"] = r["decodes"]
    return out


def expected_hit(title, labels):
    want = EXPECTED.get(title, [])
    return any(l.startswith(w) for l in labels for w in want)


def report(entries, results, out):
    by = {}
    for r in results:
        by.setdefault(r["title"], []).append(r)
    lines = ["# sigidwiki benchmark", "",
             "Samples of the Signal Identification Wiki played through hf_classifier (USB audio made into IQ).", ""]
    lines += ["| Entry | Samples | Decoded as expected | Other labels |", "|---|---|---|---|"]
    hits = total = 0
    for t in EXPECTED:
        rs = [r for r in by.get(t, []) if "error" not in r]
        ok = [r for r in rs if expected_hit(t, r["labels"])]
        other = sorted({l for r in rs for l in r["labels"] if not expected_hit(t, [l])})
        hits += len(ok)
        total += len(rs)
        lines.append("| %s | %d | %d | %s |" % (t, len(rs), len(ok), ", ".join(other) or "-"))
    review = [r for t, rs in by.items() if t not in EXPECTED for r in rs if r.get("labels")]
    tested_other = [r for t, rs in by.items() if t not in EXPECTED for r in rs if "error" not in r]
    lines += ["", "Expected entries: %d of %d samples decoded as expected." % (hits, total),
              "Other entries: %d samples of %d entries; %d with a label (to review below)." % (
                  len(tested_other), len({r["title"] for r in tested_other}), len(review)), ""]
    if review:
        lines += ["## Labels on other entries (false, or a decoded mode inside the recording)", ""]
        for r in sorted(review, key=lambda r: r["title"]):
            text = " / ".join("%s: %s" % (d[0], d[2][:60]) for d in r["decodes"][:3])
            lines.append("- **%s**, `%s`: %s. %s" % (r["title"], r["file"], ", ".join(r["labels"]), text))
        lines.append("")
    # The catalogue matcher: is a right mode among the modes the strongest
    # emissions (up to three) were found consistent with?
    lines += ["## Catalogue matches", "",
              "For wiki entries of catalogued modes: samples whose strongest emissions (up to three in each of the two runs) list a right mode",
              "among the modes they are consistent with (\"listed\"), first in that list (\"first\"), with its catalogued frame",
              "period agreeing with the measured one (\"frame\"); and how long the lists were.", "",
              "| Entry | Samples | Listed | First | Frame | Mean list length |", "|---|---|---|---|---|---|"]
    tot = [0, 0, 0, 0]
    for t in sorted(CATALOGUE_OF):
        rs = [r for r in by.get(t, []) if "error" not in r]
        if not rs:
            continue
        want = set(CATALOGUE_OF[t])
        listed = first = framed = 0
        lengths = []
        for r in rs:
            lists = [row[6] for row in r.get("strongest", []) if len(row) > 6 and row[6]]
            if any(want & set(l) for l in lists):
                listed += 1
            if any(l and l[0] in want for l in lists):
                first += 1
            if any(want & set(row[7]) for row in r.get("strongest", []) if len(row) > 7 and row[7]):
                framed += 1
            lengths += [len(l) for l in lists]
        tot[0] += len(rs); tot[1] += listed; tot[2] += first; tot[3] += framed
        mean = "%.1f" % (sum(lengths) / len(lengths)) if lengths else "-"
        lines.append("| %s | %d | %d | %d | %d | %s |" % (t, len(rs), listed, first, framed, mean))
    lines += ["", "In all: %d samples, a right mode listed for %d, first for %d, with its frame for %d." % tuple(tot), ""]
    lines += ["## Features measured on every entry's strongest emission", "",
              "What the feature meter (core/src/features.cpp) measured, for checking it against the wiki's description.", ""]
    for t in sorted(by):
        for r in sorted(by[t], key=lambda r: r["file"]):
            parts = []
            for noise, rows in sorted(r.get("strongest_by_noise", {}).items()):
                if rows and rows[0][5]:
                    cons = rows[0][6] if len(rows[0]) > 6 else []
                    parts.append("%s: %s%s" % (noise, rows[0][5], " (consistent with: %s)" % ", ".join(cons) if cons else ""))
            if parts:
                lines.append("- %s, `%s`: %s" % (t, r["file"], "; ".join(parts)))
    lines += ["", "## Per sample, expected entries", ""]
    for t in EXPECTED:
        for r in sorted(by.get(t, []), key=lambda r: r["file"]):
            mark = "error: " + r["error"] if "error" in r else ("ok" if expected_hit(t, r["labels"]) else "missed")
            text = " / ".join("%s: %s" % (d[0], d[2][:50]) for d in r.get("decodes", [])[:2])
            if mark == "missed":
                text = "strongest emissions (lo, hi Hz, SNR dB, s, label, features): %s" % r.get("strongest")
            lines.append("- %s, `%s` (%s s): %s %s %s" % (t, r["file"], r.get("seconds", "?"), mark,
                                                          json.dumps(r.get("labels", {})), text))
    errors = [r for r in results if "error" in r]
    lines += ["", "%d samples could not be used (download or format)." % len(errors)]
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return hits, total, len(review)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--classifier", required=True, help="path to hf_classifier")
    ap.add_argument("--cache", default="sigidwiki_cache")
    ap.add_argument("--entries", default="", help="only entries whose title matches this regular expression")
    ap.add_argument("--bands", default="HF,MF,LF,VLF", help="wiki categories to include (any of)")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 4), help="parallel runs (each needs up to ~1 GB)")
    ap.add_argument("--report", default="sigidwiki_report.md")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    entries = fetch_entries(a.cache)
    bands = {b.strip() for b in a.bands.split(",") if b.strip()}
    chosen = {t: e for t, e in entries.items()
              if (not bands or bands & set(e["categories"]) or t in EXPECTED) and re.search(a.entries, t)}
    jobs = [(t, f) for t, e in sorted(chosen.items()) for f in e["files"]]
    print("%d entries, %d samples" % (len(chosen), len(jobs)), flush=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run_sample, a.classifier, a.cache, t, f) for t, f in jobs]
        for i, fu in enumerate(concurrent.futures.as_completed(futs), 1):
            results.append(fu.result())
            if i % 50 == 0:
                print("%d/%d" % (i, len(jobs)), flush=True)
    json.dump(results, open(os.path.splitext(a.report)[0] + ".json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    hits, total, review = report(entries, results, a.report)
    print("expected entries: %d/%d samples decoded; %d labels on other entries to review; report: %s" % (
        hits, total, review, a.report))


if __name__ == "__main__":
    main()
