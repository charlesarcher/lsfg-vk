#!/usr/bin/env python3
"""
bench_permutations.py - Automated exhaustive benchmarking harness for lsfg-vk.

Measures and collects full statistical distribution (p10, p50, p90, p99, mean, std)
of click-to-display frame presentation latency for:
  1. Native Baselines (RX 9070 XT, RX 9060 XT, Intel Arrow Lake iGPU)
  2. Single-GPU Internal Doubling
  3. Dual-GPU Decoupled PCIe DMA Offload (directed GPU -> GPU pairs)

Usage:
    python3 bench_permutations.py [--seconds 6] [--preset bound]
"""

import argparse
import os
import re
import subprocess
import sys
import time
import numpy as np

def compute_distribution(vals):
    if not vals:
        return {"p10": 0.0, "p50": 0.0, "p90": 0.0, "p99": 0.0, "mean": 0.0, "std": 0.0, "count": 0}
    arr = np.array(vals)
    return {
        "p10": float(np.percentile(arr, 10)),
        "p50": float(np.percentile(arr, 50)),
        "p90": float(np.percentile(arr, 90)),
        "p99": float(np.percentile(arr, 99)),
        "mean": float(np.mean(arr)),
        "std": float(np.std(arr)),
        "count": len(arr),
    }

def main():
    parser = argparse.ArgumentParser(description="lsfg-vk Topology Permutation Benchmarker")
    parser.add_argument("--seconds", type=int, default=6, help="Test duration per run (default: 6)")
    parser.add_argument("--preset", type=str, default="bound", help="FurMark preset (default: bound)")
    args = parser.parse_args()

    repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    test_furmark_sh = os.path.join(repo_dir, "test_furmark.sh")

    env_base = os.environ.copy()
    env_base["DISPLAY"] = env_base.get("DISPLAY", ":0")
    env_base["WAYLAND_DISPLAY"] = env_base.get("WAYLAND_DISPLAY", "wayland-0")
    env_base["XAUTHORITY"] = env_base.get("XAUTHORITY", "/run/user/1000/xauth_xTNaei")
    env_base["VK_LAYER_PATH"] = os.path.join(repo_dir, "build/lsfg-vk-layer")
    env_base["LSFGVK_CONFIG"] = os.path.expanduser("~/.config/lsfg-vk/conf.toml")

    gpus = [
        {"name": "RX 9070 XT", "vk_idx": 1, "profile_key": "9070"},
        {"name": "RX 9060 XT", "vk_idx": 0, "profile_key": "9060"},
        {"name": "Intel ARL iGPU", "vk_idx": 2, "profile_key": "intel"},
    ]

    print("================================================================================")
    print("  LSFG-VK TOPOLOGY BENCHMARK & LATENCY HISTOGRAM COLLECTOR")
    print(f"  Preset: {args.preset} | Duration: {args.seconds}s per run")
    print("================================================================================\n")

    # 1. Native Baselines
    print("--- 1. NATIVE BASELINES ---")
    for g in gpus:
        env = env_base.copy()
        env.pop("VK_INSTANCE_LAYERS", None)
        env.pop("LSFGVK_PROFILE", None)
        env["RENDER_GPU_INDEX"] = str(g["vk_idx"])

        cmd = [test_furmark_sh, "baseline", str(args.seconds), args.preset]
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=repo_dir)
        raw_out, _ = p.communicate(timeout=90)
        out_str = raw_out.decode("utf-8", errors="replace")

        m = re.search(r'fps min/avg/max:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)', out_str)
        fps = int(m.group(2)) if m else 0
        print(f"  Native {g['name']:<15}: {fps:>4} FPS (frametime ~{1000.0/max(fps,1):.2f} ms)")

    # 2. Dual-GPU Decoupled Offload
    print("\n--- 2. DUAL-GPU DECOUPLED OFFLOAD COMBINATIONS ---")
    for r in gpus:
        for c in gpus:
            if r["name"] == c["name"]:
                continue
            key = f"{r['name']} -> {c['name']}"
            subprocess.run(["killall", "-9", "lsfg-vk-app"], stderr=subprocess.DEVNULL)
            time.sleep(0.5)

            cmd = [test_furmark_sh, "doubled", str(args.seconds), args.preset]
            test_env = env_base.copy()
            test_env["RENDER_GPU_INDEX"] = str(r["vk_idx"])
            test_env["APP_PROFILE"] = f"app-{c['profile_key']}"
            test_env["GAME_PROFILE"] = f"ext-{c['profile_key']}"

            p = subprocess.Popen(cmd, env=test_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=repo_dir)
            raw_out, _ = p.communicate(timeout=90)
            out_str = raw_out.decode("utf-8", errors="replace")

            m_out = re.search(r'out=(/tmp/lsfg-furmark-\S+)', out_str)
            out_dir = m_out.group(1) if m_out else None

            game_fps = 0
            m_fps = re.search(r'fps min/avg/max:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)', out_str)
            if m_fps:
                game_fps = int(m_fps.group(2))

            real_lats = []
            gen_lats = []
            if out_dir and os.path.exists(f"{out_dir}/app.log"):
                app_text = open(f"{out_dir}/app.log", errors="replace").read()
                real_lats = [float(x) for x in re.findall(r'MEASURED LATENCY: REAL present slot \d+ latency ([\d\.]+) ms', app_text)]
                gen_lats = [float(x) for x in re.findall(r'MEASURED LATENCY: GEN present slot \d+ latency ([\d\.]+) ms', app_text)]

            r_dist = compute_distribution(real_lats)
            g_dist = compute_distribution(gen_lats)
            presents_per_sec = (len(real_lats) + len(gen_lats)) / float(args.seconds)

            print(f"\n  [{key}]")
            print(f"    Base Render FPS    : {game_fps} FPS")
            print(f"    Total Presentation : {presents_per_sec:.1f} presents/s (Real: {len(real_lats)}, Gen: {len(gen_lats)})")
            print(f"    REAL Frame Latency : p10={r_dist['p10']:.2f}ms | p50={r_dist['p50']:.2f}ms | p90={r_dist['p90']:.2f}ms | p99={r_dist['p99']:.2f}ms | mean={r_dist['mean']:.2f}ms")
            print(f"    GEN  Frame Latency : p10={g_dist['p10']:.2f}ms | p50={g_dist['p50']:.2f}ms | p90={g_dist['p90']:.2f}ms | p99={g_dist['p99']:.2f}ms | mean={g_dist['mean']:.2f}ms")

            subprocess.run(["killall", "-9", "lsfg-vk-app"], stderr=subprocess.DEVNULL)

if __name__ == "__main__":
    main()
