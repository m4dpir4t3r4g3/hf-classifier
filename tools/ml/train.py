#!/usr/bin/env python3
"""Compares models for the signal-family classifier and exports the chosen
one for the C++ program.

  tools/ml/train.py --data family_data.jsonl [--report report.md] [--export catalogue/family_model.json]
                    [--models rule,forest,svm,mlp,cnn,resnet,transformer] [--seed 1]

Protocol (so that the numbers mean something):
  * Wiki pages are kept whole: five folds over pages (stratified by family),
    every window of every recording of a page in the same fold. A model is
    never scored on a system it saw in training.
  * Models are trained on the clean windows of four folds and scored on the
    fifth: its clean windows, and the same windows with noise added for
    10 dB and 0 dB in 3 kHz. "forest+noise" also trains on the noisy ones.
  * Scores: accuracy over windows, over recordings (the windows' summed
    probabilities), and the macro-averaged F1 (families are unequal in
    size); with the standard deviation over the folds.
  * The rule-based meter (core/src/features.cpp) is scored on the same
    windows; it has no sweep/pulse or analogue family, so it is also
    compared on the five families it knows.
  * Confidence: for the exported forest, how often it is right among the
    predictions it makes above each confidence, and how many it makes.

Needs numpy and scikit-learn; cnn, resnet and transformer need PyTorch.
"""
import argparse
import collections
import hashlib
import json
import sys

import numpy as np

FAMILIES = ["FSK", "MFSK", "PSK", "multi-carrier", "OOK", "sweep/pulse", "analogue"]
RULE_FAMILIES = FAMILIES[:5]


def load(path):
    rows = [json.loads(l) for l in open(path, encoding="utf-8")]
    rows = [r for r in rows if r["label"] in FAMILIES]
    X = np.array([r["vector"] for r in rows], dtype=np.float32)
    S = np.array([r["spectrogram"] for r in rows], dtype=np.float32).reshape(-1, 64, 48)
    y = np.array([FAMILIES.index(r["label"]) for r in rows])
    groups = np.array([r["entry"] for r in rows])
    files = np.array([r["entry"] + "/" + r["file"] for r in rows])
    snr = np.array([-1 if r["snr"] is None else r["snr"] for r in rows])
    rule = np.array([FAMILIES.index(r["rule"]) if r["rule"] in FAMILIES else -1 for r in rows])
    return rows, X, S, y, groups, files, snr, rule


def macro_f1(y, p, classes):
    f = []
    for c in classes:
        tp = np.sum((y == c) & (p == c))
        fp = np.sum((y != c) & (p == c))
        fn = np.sum((y == c) & (p != c))
        if tp + fn == 0:
            continue
        f.append(0.0 if tp == 0 else 2 * tp / (2 * tp + fp + fn))
    return float(np.mean(f))


def torch_model(kind, seed):
    import torch
    import torch.nn as nn
    torch.manual_seed(seed)

    class Cnn(nn.Module):
        def __init__(self):
            super().__init__()
            self.f = nn.Sequential(nn.Conv2d(1, 16, 3, padding=1), nn.BatchNorm2d(16), nn.ReLU(), nn.MaxPool2d(2),
                                   nn.Conv2d(16, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(), nn.MaxPool2d(2),
                                   nn.Conv2d(32, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(), nn.AdaptiveAvgPool2d(1))
            self.h = nn.Sequential(nn.Flatten(), nn.Dropout(0.3), nn.Linear(64, len(FAMILIES)))

        def forward(self, x):
            return self.h(self.f(x.unsqueeze(1)))

    class Block(nn.Module):
        def __init__(self, c):
            super().__init__()
            self.a = nn.Sequential(nn.Conv2d(c, c, 3, padding=1), nn.BatchNorm2d(c), nn.ReLU(),
                                   nn.Conv2d(c, c, 3, padding=1), nn.BatchNorm2d(c))

        def forward(self, x):
            return torch.relu(x + self.a(x))

    class ResNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.f = nn.Sequential(nn.Conv2d(1, 24, 3, padding=1), nn.BatchNorm2d(24), nn.ReLU(), Block(24), nn.MaxPool2d(2),
                                   Block(24), nn.MaxPool2d(2), Block(24), nn.AdaptiveAvgPool2d(1))
            self.h = nn.Sequential(nn.Flatten(), nn.Dropout(0.3), nn.Linear(24, len(FAMILIES)))

        def forward(self, x):
            return self.h(self.f(x.unsqueeze(1)))

    class Transformer(nn.Module):  # the 64 time steps as tokens of 48 frequency bins
        def __init__(self):
            super().__init__()
            self.inp = nn.Linear(48, 48)
            self.pos = nn.Parameter(torch.zeros(1, 64, 48))
            layer = nn.TransformerEncoderLayer(48, 4, 96, dropout=0.2, batch_first=True)
            self.enc = nn.TransformerEncoder(layer, 2)
            self.h = nn.Linear(48, len(FAMILIES))

        def forward(self, x):
            return self.h(self.enc(self.inp(x) + self.pos).mean(1))

    return {"cnn": Cnn, "resnet": ResNet, "transformer": Transformer}[kind]()


def fit_torch(kind, S, y, seed, epochs=40):
    import torch
    net = torch_model(kind, seed)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-3)
    w = torch.tensor(len(y) / (len(FAMILIES) * np.maximum(1, np.bincount(y, minlength=len(FAMILIES)))), dtype=torch.float32)
    loss = torch.nn.CrossEntropyLoss(weight=w)
    Xt, yt = torch.tensor(S), torch.tensor(y)
    g = torch.Generator().manual_seed(seed)
    net.train()
    for _ in range(epochs):
        order = torch.randperm(len(yt), generator=g)
        for i in range(0, len(order), 64):
            b = order[i:i + 64]
            opt.zero_grad()
            loss(net(Xt[b]), yt[b]).backward()
            opt.step()
    net.eval()
    return lambda Z: torch.softmax(net(torch.tensor(Z)), 1).detach().numpy()


def fit(kind, X, S, y, seed):
    from sklearn.ensemble import RandomForestClassifier
    from sklearn.neural_network import MLPClassifier
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    from sklearn.svm import SVC
    if kind in ("forest", "forest+noise"):
        m = RandomForestClassifier(300, min_samples_leaf=2, class_weight="balanced_subsample", random_state=seed, n_jobs=-1).fit(X, y)
        return lambda Xn, Sn: full(m, Xn)
    if kind == "forest-small+noise":  # as exported
        m = small_forest(40, seed).fit(X, y)
        return lambda Xn, Sn: full(m, Xn)
    if kind == "svm":
        m = make_pipeline(StandardScaler(), SVC(C=10, gamma="scale", probability=True, class_weight="balanced", random_state=seed)).fit(X, y)
        return lambda Xn, Sn: full(m, Xn)
    if kind == "mlp":
        m = make_pipeline(StandardScaler(), MLPClassifier((128, 64), alpha=1e-2, max_iter=600, random_state=seed)).fit(X, y)
        return lambda Xn, Sn: full(m, Xn)
    p = fit_torch(kind, S, y, seed)
    return lambda Xn, Sn: p(Sn)


def small_forest(trees, seed):
    from sklearn.ensemble import RandomForestClassifier
    return RandomForestClassifier(trees, min_samples_leaf=4, max_depth=10, class_weight="balanced_subsample", random_state=seed, n_jobs=-1)


def full(model, X):
    """predict_proba over all the families (a fold may lack one)."""
    p = np.zeros((len(X), len(FAMILIES)), dtype=np.float64)
    classes = model.classes_ if hasattr(model, "classes_") else model[-1].classes_
    p[:, classes] = model.predict_proba(X)
    return p


def per_file(prob, files):
    """Each window gets its recording's summed probabilities."""
    out = np.zeros_like(prob)
    for f in np.unique(files):
        i = files == f
        out[i] = prob[i].sum(0)
    return out


def export_forest(model, path, version):
    """The text format of core/include/hfc/family_model.hpp."""
    out = ["hfc-family-forest 1", "version " + version, "families " + "|".join(FAMILIES), "features %d" % model.n_features_in_]
    for est in model.estimators_:
        t = est.tree_
        out.append("tree %d" % t.node_count)
        for i in range(t.node_count):
            if t.children_left[i] < 0:
                v = np.zeros(len(FAMILIES))
                v[est.classes_.astype(int)] = t.value[i, 0, :]
                v = v / max(1e-12, v.sum())
                out.append("-1 " + " ".join("%.3g" % x for x in v))
            else:
                out.append("%d %.6g %d %d" % (t.feature[i], t.threshold[i], t.children_left[i], t.children_right[i]))
    open(path, "w", encoding="ascii", newline="\n").write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--data", required=True)
    ap.add_argument("--report", default="family_model_report.md")
    ap.add_argument("--export", default="")
    ap.add_argument("--models", default="rule,forest,forest+noise,forest-small+noise,svm,mlp,cnn,resnet,transformer")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--trees", type=int, default=40, help="trees in the exported forest")
    a = ap.parse_args()
    from sklearn.model_selection import StratifiedGroupKFold
    rows, X, S, y, groups, files, snr, rule = load(a.data)
    clean = snr == -1
    lines = ["# Signal-family model: comparison", "",
             "%d windows of %d recordings of %d wiki pages; clean windows per family: %s." % (
                 clean.sum(), len(set(files)), len(set(groups)),
                 ", ".join("%s %d" % (f, np.sum(y[clean] == i)) for i, f in enumerate(FAMILIES))), "",
             "Five folds over pages; trained on clean windows (\"+noise\": on the noisy ones too), scored on the held-out pages.",
             "Mean and standard deviation over the folds, in percent.", "",
             "| Model | Windows, clean | Recordings, clean | Macro F1, clean | Windows, 10 dB | Windows, 0 dB |", "|---|---|---|---|---|---|"]
    folds = list(StratifiedGroupKFold(5, shuffle=True, random_state=a.seed).split(X, y, groups))
    conf_rows = []  # (confidence, right) of the forest+noise model on clean held-out windows
    confusion = np.zeros((len(FAMILIES), len(FAMILIES)), dtype=int)
    for kind in a.models.split(","):
        sc = collections.defaultdict(list)
        for tr, te in folds:
            if kind == "rule":
                pred = {c: rule[te][snr[te] == c] for c in (-1, 10, 0)}
                prob = None
            else:
                tri = tr if kind.endswith("+noise") else tr[snr[tr] == -1]
                try:
                    predict = fit(kind, X[tri], S[tri], y[tri], a.seed)
                except ImportError as e:
                    print("skipping %s: %s" % (kind, e), file=sys.stderr)
                    sc = None
                    break
                prob = {c: predict(X[te][snr[te] == c], S[te][snr[te] == c]) for c in (-1, 10, 0)}
                pred = {c: p.argmax(1) for c, p in prob.items()}
            for c, name in ((-1, "clean"), (10, "10"), (0, "0")):
                yt = y[te][snr[te] == c]
                sc[name].append(100 * np.mean(pred[c] == yt))
            yt = y[te][snr[te] == -1]
            sc["f1"].append(100 * macro_f1(yt, pred[-1], range(len(FAMILIES))))
            if prob is not None:
                pf = per_file(prob[-1], files[te][snr[te] == -1]).argmax(1)
                sc["file"].append(100 * np.mean(pf == yt))
                if kind == "forest-small+noise":
                    conf_rows += list(zip(prob[-1].max(1), pred[-1] == yt))
                    for t, p in zip(yt, pred[-1]):
                        confusion[t, p] += 1
            else:
                known = np.isin(yt, range(len(RULE_FAMILIES)))
                sc["known"].append(100 * np.mean(pred[-1][known] == yt[known]))
        if sc is None:
            continue
        cell = lambda k: "%.1f ± %.1f" % (np.mean(sc[k]), np.std(sc[k])) if sc[k] else "-"
        lines.append("| %s | %s | %s | %s | %s | %s |" % (kind, cell("clean"), cell("file"), cell("f1"), cell("10"), cell("0")))
        if kind == "rule":
            lines.append("| rule, on the five families it knows | %s | - | - | - | - |" % cell("known"))
        print(lines[-1], flush=True)
    if conf_rows:
        c = np.array([r[0] for r in conf_rows]); ok = np.array([r[1] for r in conf_rows])
        lines += ["", "## Confidence of forest-small+noise, the exported kind (clean held-out windows)", "",
                  "| Predictions with confidence at least | Share of all windows | Right among them |", "|---|---|---|"]
        for th in (0.0, 0.4, 0.5, 0.6, 0.7, 0.8):
            m = c >= th
            lines.append("| %.1f | %.0f %% | %.0f %% |" % (th, 100 * m.mean(), 100 * ok[m].mean() if m.any() else 0))
        lines += ["", "## Confusion of forest-small+noise (clean held-out windows; rows: the page's family, columns: predicted)", "",
                  "| | " + " | ".join(FAMILIES) + " |", "|---|" + "---|" * len(FAMILIES)]
        for i, f in enumerate(FAMILIES):
            lines.append("| %s | %s |" % (f, " | ".join(str(v) for v in confusion[i])))
    open(a.report, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("report: " + a.report)
    if a.export:
        m = small_forest(a.trees, a.seed).fit(X, y)
        digest = hashlib.sha256(open(a.data, "rb").read()).hexdigest()[:12]
        export_forest(m, a.export, "family-forest-1 (%d trees, data %s)" % (a.trees, digest))
        print("exported: " + a.export)


if __name__ == "__main__":
    main()
