#!/usr/bin/env python3
"""
bench_permutations.py - Automated GPU Topology Permutation Benchmarker for lsfg-vk.

Measures and reports the full statistical latency distribution (p10, p50, p90, p99, mean, std)
of frame presentation across:
  1. Native Baselines (per-GPU render throughput without layer)
  2. One-Way Decoupled Dual-GPU Offload pairs (Render GPU -> Present GPU)

Usage:
    python3 bench_permutations.py [--seconds 6] [--preset bound] [--json output.json]
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import time
from typing import Dict, List, Any


def compute_percentile(data: List[float], percentile: float) -> float:
    if not data:
        return 0.0
    s = sorted(data)
    k = (len(s) - 1) * (percentile / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    d0 = s[int(f)] * (c - k)
    d1 = s[int(c)] * (k - f)
    return float(d0 + d1)


def compute_distribution(vals: List[float]) -> Dict[str, Any]:
    if not vals:
        return {
            "count": 0,
            "mean": 0.0,
            "std": 0.0,
            "p10": 0.0,
            "p50": 0.0,
            "p90": 0.0,
            "p99": 0.0,
        }
    n = len(vals)
    mean = sum(vals) / n
    var = sum((x - mean) ** 2 for x in vals) / n if n > 1 else 0.0
    std = math.sqrt(var)
    return {
        "count": n,
        "mean": round(mean, 2),
        "std": round(std, 2),
        "p10": round(compute_percentile(vals, 10), 2),
        "p50": round(compute_percentile(vals, 50), 2),
        "p90": round(compute_percentile(vals, 90), 2),
        "p99": round(compute_percentile(vals, 99), 2),
    }


def main():
    parser = argparse.ArgumentParser(description="lsfg-vk Topology Permutation Benchmarker")
    parser.add_argument("--seconds", type=int, default=6, help="Test duration per cell in seconds (default: 6)")
    parser.add_argument("--preset", type=str, default="bound", help="FurMark preset: bound|light|panel|heavy (default: bound)")
    parser.add_argument("--transport", type=str, choices=["shm", "dmabuf"], default="shm", help="Transport mechanism: shm (CPU copy) or dmabuf (zero-copy) (default: shm)")
    parser.add_argument("--json", type=str, default="", help="Optional path to write results as JSON")
    args = parser.parse_args()

    # Dynamic repository root discovery
    repo_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bench_furmark_sh = os.path.join(repo_dir, "tools", "benchmarks", "bench_furmark.sh")

    if not os.path.isfile(bench_furmark_sh):
        bench_furmark_sh = os.path.join(repo_dir, "test_furmark.sh")

    env_base = os.environ.copy()
    env_base["DISPLAY"] = env_base.get("DISPLAY", ":0")
    env_base["WAYLAND_DISPLAY"] = env_base.get("WAYLAND_DISPLAY", "wayland-0")
    env_base["VK_LAYER_PATH"] = os.path.join(repo_dir, "build", "lsfg-vk-layer")
    env_base["LSFGVK_CONFIG"] = os.path.expanduser("~/.config/lsfg-vk/conf.toml")
    # Pure numeric measurement without overlay noise
    env_base["MANGOHUD"] = "0"

    gpus = [
        {"name": "RX 9070 XT", "vk_idx": 1, "profile_key": "9070"},
        {"name": "RX 9060 XT", "vk_idx": 0, "profile_key": "9060"},
        {"name": "Intel ARL iGPU", "vk_idx": 2, "profile_key": "intel"},
    ]

    results: Dict[str, Any] = {
        "preset": args.preset,
        "seconds_per_cell": args.seconds,
        "baselines": {},
        "pairs": {},
    }

    print("================================================================================")
    print("  LSFG-VK TOPOLOGY BENCHMARK & LATENCY DISTRIBUTION COLLECTOR")
    print(f"  Preset: {args.preset} | Duration: {args.seconds}s per run")
    print("================================================================================\n")

    # 1. Native Baselines
    print("--- 1. NATIVE BASELINES ---")
    for g in gpus:
        env = env_base.copy()
        env.pop("VK_INSTANCE_LAYERS", None)
        env.pop("LSFGVK_PROFILE", None)
        env["RENDER_GPU_INDEX"] = str(g["vk_idx"])

        cmd = [bench_furmark_sh, "baseline", str(args.seconds), args.preset]
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=repo_dir)
        raw_out, _ = p.communicate(timeout=120)
        out_str = raw_out.decode("utf-8", errors="replace")

        m = re.search(r'FPS \(min/avg/max\)\s*:\s*\d+\s*/\s*(\d+)\s*/\s*\d+', out_str)
        fps = int(m.group(1)) if m else 0
        frametime = round(1000.0 / max(fps, 1), 2)
        results["baselines"][g["name"]] = {"fps": fps, "frametime_ms": frametime}
        print(f"  Native {g['name']:<15}: {fps:>4} FPS (frametime ~{frametime:.2f} ms)")

    # 2. Dual-GPU Decoupled Offload Combinations
    print("\n--- 2. DUAL-GPU DECOUPLED OFFLOAD COMBINATIONS ---")
    for r in gpus:
        for c in gpus:
            if r["name"] == c["name"]:
                continue
            pair_key = f"{r['name']} -> {c['name']}"
            subprocess.run(["pkill", "-x", "lsfg-vk-app"], stderr=subprocess.DEVNULL)
            time.sleep(0.5)

            cmd = [bench_furmark_sh, "doubled", str(args.seconds), args.preset, args.transport]
            test_env = env_base.copy()
            test_env["RENDER_GPU_INDEX"] = str(r["vk_idx"])
            test_env["LSFGVK_APP_PROFILE"] = f"app-{c['profile_key']}"
            test_env["LSFGVK_PROFILE"] = f"furmark-oneway"

            p = subprocess.Popen(cmd, env=test_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=repo_dir)
            raw_out, _ = p.communicate(timeout=120)
            out_str = raw_out.decode("utf-8", errors="replace")

            m_out = re.search(r'Artifact directory:\s*(\S+)', out_str)
            if not m_out:
                m_out = re.search(r'log:\s*(\S+)/doubled\.log', out_str)
            out_dir = m_out.group(1) if m_out else None

            game_fps = 0
            m_fps = re.search(r'FPS \(min/avg/max\)\s*:\s*\d+\s*/\s*(\d+)\s*/\s*\d+', out_str)
            if m_fps:
                game_fps = int(m_fps.group(1))

            real_lats: List[float] = []
            gen_lats: List[float] = []
            if out_dir and os.path.isdir(out_dir):
                app_log_path = os.path.join(out_dir, "app.log")
                if os.path.isfile(app_log_path):
                    with open(app_log_path, "r", errors="replace") as f:
                        app_text = f.read()
                    real_lats = [float(x) for x in re.findall(r'REAL present slot \d+ latency ([\d\.]+) ms', app_text)]
                    gen_lats = [float(x) for x in re.findall(r'GEN present slot \d+ latency ([\d\.]+) ms', app_text)]

            r_dist = compute_distribution(real_lats)
            g_dist = compute_distribution(gen_lats)
            presents_per_sec = round((len(real_lats) + len(gen_lats)) / float(max(args.seconds, 1)), 1)

            results["pairs"][pair_key] = {
                "render_gpu": r["name"],
                "present_gpu": c["name"],
                "base_render_fps": game_fps,
                "presents_per_sec": presents_per_sec,
                "real_latency": r_dist,
                "gen_latency": g_dist,
            }

            print(f"\n  [{pair_key}]")
            print(f"    Base Render FPS    : {game_fps} FPS")
            print(f"    Total Presentation : {presents_per_sec:.1f} presents/s (Real: {len(real_lats)}, Gen: {len(gen_lats)})")
            print(f"    REAL Frame Latency : p10={r_dist['p10']:.2f}ms | p50={r_dist['p50']:.2f}ms | p90={r_dist['p90']:.2f}ms | p99={r_dist['p99']:.2f}ms | mean={r_dist['mean']:.2f}ms")
            print(f"    GEN  Frame Latency : p10={g_dist['p10']:.2f}ms | p50={g_dist['p50']:.2f}ms | p90={g_dist['p90']:.2f}ms | p99={g_dist['p99']:.2f}ms | mean={g_dist['mean']:.2f}ms")

            subprocess.run(["pkill", "-x", "lsfg-vk-app"], stderr=subprocess.DEVNULL)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nResults successfully written to {args.json}")


if __name__ == "__main__":
    main()
