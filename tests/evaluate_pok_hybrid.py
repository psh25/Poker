"""Host-only first-pass evaluation of the hybrid OpenMV pipeline on pok/*.bmp.

The BMP files are IDE screenshots, so saturated diagnostic rectangles are
inpainted before evaluation. Generated PGM files are previews only and must
not be copied into the OpenMV template bank.
"""
import importlib
import json
import os
from pathlib import Path
import sys
import time
import types

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_cards_fast import HostImage

V = None  # Set by main() after the OpenMV host shim has been installed.


def install_host_api():
    sys.modules["image"] = types.SimpleNamespace(Image=HostImage, GRAYSCALE=0, AREA=0)
    sys.modules["machine"] = types.SimpleNamespace(Pin=object, UART=object)
    time.ticks_ms = lambda: int(time.monotonic() * 1000)
    time.ticks_diff = lambda a, b: a - b
    time.sleep_ms = lambda ms: time.sleep(ms / 1000)
    os.sync = lambda: None


def remove_ide_overlays(bgr):
    """Remove saturated red/blue/green/magenta/cyan annotation strokes."""
    hi = bgr.max(axis=2)
    lo = bgr.min(axis=2)
    saturated = (hi > 210) & ((hi - lo) > 150)
    mask = saturated.astype(np.uint8) * 255
    mask = cv2.dilate(mask, np.ones((3, 3), np.uint8))
    return cv2.inpaint(bgr, mask, 3, cv2.INPAINT_TELEA)


def labels_from_name(path):
    stem = path.stem
    if stem.startswith("joker_"):
        return "joker", stem.split("_", 1)[1]
    rank, suit = stem.split("_", 1)
    return rank, suit


def best_other(patch, samples, source, limit=2):
    by_label = {}
    for sample in samples:
        if sample["source"] == source:
            continue
        by_label.setdefault(sample["label"], []).append(sample)
    scores = []
    for label, choices in by_label.items():
        score = max(V.difference_score(patch, item["patch"])
                    for item in choices[:limit])
        scores.append((score, label))
    scores.sort(reverse=True)
    if not scores:
        return None, -1.0, 0.0
    second = scores[1][0] if len(scores) > 1 else -1.0
    return scores[0][1], scores[0][0], scores[0][0] - second


def save_seed_preview(samples, destination):
    """Choose up to two same-label medoids; still screenshot-derived only."""
    manifest = {"warning": "preview only; do not install as OpenMV templates",
                "groups": {}}
    for group, group_samples in samples.items():
        group_dir = destination / group
        group_dir.mkdir(parents=True, exist_ok=True)
        for old_patch in group_dir.glob("*.pgm"):
            old_patch.unlink()
        labels = sorted(set(item["label"] for item in group_samples))
        manifest["groups"][group] = {}
        for label in labels:
            choices = [item for item in group_samples if item["label"] == label]
            ranked = []
            for item in choices:
                peers = [other for other in choices if other is not item]
                centrality = (sum(V.difference_score(item["patch"], peer["patch"])
                                  for peer in peers) / len(peers)) if peers else 1.0
                ranked.append((centrality, item))
            ranked.sort(key=lambda pair: pair[0], reverse=True)
            selected = []
            for index, (centrality, item) in enumerate(ranked[:2]):
                filename = "%s_%02d.pgm" % (label, index)
                item["patch"].save(str(group_dir / filename))
                selected.append({"file": filename, "source": item["source"],
                                 "centrality": round(centrality, 4)})
            manifest["groups"][group][label] = selected
    (destination / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


def main():
    global V
    install_host_api()
    import cards_fast_config as C
    import cards_hybrid_core as core
    V = core
    import capture_cards_hybrid as Capture
    importlib.reload(V)
    importlib.reload(Capture)

    out_dir = ROOT / "pok_hybrid_preview"
    out_dir.mkdir(exist_ok=True)
    for old_patch in out_dir.glob("*.pgm"):
        old_patch.unlink()
    samples = {"rank": [], "suit": [], "joker": []}
    rows = []

    for path in sorted((ROOT / "pok").glob("*.bmp")):
        bgr = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
        clean = remove_ide_overlays(bgr)
        frame = HostImage(clean)
        first, second = labels_from_name(path)
        row = {"source": path.name}
        if first == "joker":
            candidates, info = V.locate(frame, "joker", Capture.CAPTURE_VISION)
            row.update({"kind": "joker", "candidates": len(candidates),
                        "diagnostics": info, "located": bool(candidates)})
            if candidates:
                chosen = candidates[0]
                patch = V.canonical_patch(frame, chosen["box"], "joker",
                                          Capture.CAPTURE_VISION)
                evidence = V.color_evidence(frame, chosen["box"], Capture.CAPTURE_VISION)
                row.update({"box": chosen["box"], "color": evidence})
                if patch is not None:
                    samples["joker"].append({"source": path.name, "label": first,
                                              "variant": second, "patch": patch})
                    patch.save(str(out_dir / (path.stem + "__joker.pgm")))
            rows.append(row)
            continue

        ranks, rank_info = V.locate(frame, "rank", Capture.CAPTURE_VISION)
        suits, suit_info = V.locate(frame, "suit", Capture.CAPTURE_VISION)
        pairs = V.layout_pairs(ranks, suits, Capture.CAPTURE_VISION, frame)
        row.update({"kind": "normal", "rank_candidates": len(ranks),
                    "suit_candidates": len(suits), "layout_pairs": len(pairs),
                    "rank_diagnostics": rank_info, "suit_diagnostics": suit_info,
                    "located": bool(pairs)})
        if pairs:
            unused, rank_candidate, suit_candidate = pairs[0]
            row["rank_box"], row["suit_box"] = rank_candidate["box"], suit_candidate["box"]
            for group, candidate, label in (("rank", rank_candidate, first),
                                             ("suit", suit_candidate, second)):
                patch = V.canonical_patch(frame, candidate["box"], group,
                                          Capture.CAPTURE_VISION)
                if patch is not None:
                    samples[group].append({"source": path.name, "label": label,
                                           "patch": patch})
                    patch.save(str(out_dir / (path.stem + "__" + group + ".pgm")))
        rows.append(row)

    cv_results = {}
    for group in ("rank", "suit"):
        tested = correct = accepted = accepted_correct = untestable = 0
        detail = []
        for sample in samples[group]:
            if not any(other["source"] != sample["source"]
                       and other["label"] == sample["label"]
                       for other in samples[group]):
                untestable += 1
                continue
            predicted, score, margin = best_other(sample["patch"], samples[group],
                                                   sample["source"])
            if predicted is None:
                continue
            tested += 1
            correct += predicted == sample["label"]
            is_accepted = (score >= Capture.CAPTURE_VISION.get(
                "accept_score", {group: 0.80}).get(group, 0.80)
                and margin >= 0.05)
            accepted += is_accepted
            accepted_correct += is_accepted and predicted == sample["label"]
            detail.append({"source": sample["source"], "actual": sample["label"],
                           "predicted": predicted, "score": round(score, 4),
                           "margin": round(margin, 4),
                           "accepted_at_0.80_margin_0.05": is_accepted,
                           "correct": predicted == sample["label"]})
        cv_results[group] = {"tested": tested, "correct": correct,
                             "accuracy": correct / tested if tested else 0,
                             "untestable_without_second_same_label": untestable,
                             "accepted": accepted,
                             "accepted_correct": accepted_correct,
                             "accepted_precision": (accepted_correct / accepted
                                                    if accepted else 0),
                             "accepted_rate": accepted / tested if tested else 0,
                             "detail": detail}

    seed_manifest = save_seed_preview(samples, ROOT / "pok_hybrid_seed_preview")
    joker_rows = [r for r in rows if r["kind"] == "joker"]
    joker_color_correct = sum(
        r.get("color", {}).get("color") == ("red" if "big" in r["source"] else "black")
        for r in joker_rows if r["located"])
    joker_similarity = None
    if len(samples["joker"]) == 2:
        joker_similarity = round(V.difference_score(samples["joker"][0]["patch"],
                                                     samples["joker"][1]["patch"]), 4)

    normals = [r for r in rows if r["kind"] == "normal"]
    jokers = [r for r in rows if r["kind"] == "joker"]
    report = {
        "warning": "IDE screenshot-derived preview; recollect templates on OpenMV from raw frames",
        "normal_images": len(normals),
        "normal_located": sum(r["located"] for r in normals),
        "joker_images": len(jokers),
        "joker_located": sum(r["located"] for r in jokers),
        "joker_color_correct": joker_color_correct,
        "joker_shape_similarity": joker_similarity,
        "cross_validation": cv_results,
        "seed_preview": seed_manifest,
        "images": rows,
    }
    report_path = ROOT / "pok_hybrid_report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "images"},
                     ensure_ascii=False, indent=2))
    print("report:", report_path)
    print("preview patches:", out_dir)
    print("selected seed preview:", ROOT / "pok_hybrid_seed_preview")


if __name__ == "__main__":
    main()
