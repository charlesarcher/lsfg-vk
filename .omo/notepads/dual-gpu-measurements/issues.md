# Issues — dual-gpu-measurements

Problems and gotchas encountered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## 2026-08-29 03:35 — F1 final verification findings (audit of 17bd0c4, fresh worktree /tmp/opencode/f1-audit)

Full report: `/tmp/opencode/f1-audit/measurements/_final-f1.md` — **VERDICT: PASS** (all 5 acceptance checks; no tolerance relaxed; one numeric sub-check deviation flagged).

- **F1-A (docs fix)**: spec/R0 sign-off misattributes the cross frame-time reference for Intel→9070XT 1440p-SDR-m2. 11.50 ms (3.22+8.28) is the **Intel→9060XT** row (analysis.md line 28); this cell's row (line 68) is **1.14+8.28 = 9.42 ms**. Re-run measured 11.6513 ms steady-state = 9.42 + ~2.2 ms debug-tool staging upload on the Intel device. Passes the spec-stated 11.50 ms at +1.32 % (±10 %). Fix the spec/sign-off row citation; no re-measurement needed.
- **F1-B (flagged deviation)**: strace aggregate off/on drm-ioctl ratio = 1.149× > 1.02× bound. Excess is entirely one-shot driver-init ioctls (36× AMDGPU_INFO + 6× AMDGPU_CTX on renderD130) in the 65-variant init profile; that profile appears in BOTH gate states (Soff, Soff2 off vs Son, Son2, Soff3 all 31-variant), zero per-frame ioctls in every run (totals frame-independent: 186@12904f = 186@12551f; 152@12914f = 152@12770f = 152@12660f), zero query/timestamp ioctls on the gate-off path, like-for-like paired ratio 1.000×. Direction is opposite to an instrumentation-contamination hypothesis. Awaiting orchestrator disposition.
- **F1-C (provenance)**: committed gate-off reference 879.27 fps (`raw/baseline/9070XT/1440p-SDR-m2`, ts 2026-08-28T23:35:53) was produced by the instrumented WIP build with the gate ON (timing.csv header + empty "timing summary" in its run.err), despite recording `commit_sha c376f8e`. The true pre-instrumentation `_instr-smoke` number remains lost.
- **F1-D (measurement protocol)**: warm-state (back-to-back) gate-off runs sit 2.5–5.2 % below the 3 h-old reference uniformly (1080p control cell −3.79 %); GPU state recovers monotonically with idle (60 s→~839, 150 s→875, 300 s→886–888). Drivers/kernel/userland proven unchanged (pacman.log: 0 driver packages in the 2026-08-28 23:34 -Syu; kernel 7.2.0 still running; power cap at 200 W max; temps 39–40 °C). Adopted fresh-state protocol: 300 s idle before each measured run → M1 887.70 (+0.96 %), M3 886.03 (+0.77 %) vs 879.27, both within ±2 %. Gate-on fresh-state cost ≈ 2.8 % (M2 862.00) — expected opt-in instrumentation cost.
- **Ops gotchas**: (1) `/mnt/windows` drvfs mount throws intermittent `open` failures on Lossless.dll under sustained use — copy the DLL locally (sha256-verified) for benchmark campaigns. (2) Non-detached `nohup … &` background scripts get killed by shell-session process-group cleanup — use `setsid nohup … < /dev/null` for long audit sequences. (3) /tmp is a 16 GB tmpfs — full-payload 1440p DDS frames (14.7 MB each) must be generated on /home.

---

## 2026-08-29 04:10 — F2 final verification findings (read-only audit of 17bd0c4, worktree /tmp/opencode/measure-timing)

Full report: `/tmp/opencode/measure-timing/measurements/_final-f2.md` — **VERDICT: APPROVE** (all 5 checks pass; 3 minor + 4 informational findings, no failures).

- **F2-1 (MINOR, traceability)**: copybench raw outputs NOT committed anywhere (`git ls-files | grep -i copybench` → source files only). Cited source for all copybench-derived numbers (analysis.md T2 L79-98, RFC draft L32-37/⁵, Guide L144 ff., Troubleshooting L44, Configuration L28-29) is "learnings.md lines 119-126" = the uncommitted notepad, which holds only approximate values. Mitigation: exact values + derivation formulas ARE committed in `analyze.py` `COPYBENCH_RESULTS` (L43-73); `python3 analyze.py` reproduces analysis.md value-exact (verified by /tmp regeneration + sort-diff identical); 1.05 GB/s anchor hand-recomputed in committed `raw/_analysis-audit.md` #2. 12/24 T2 rows are extrapolations (×1.78 pixel-ratio / ×2 HDR / "scaled from SDR" / bimodal midpoint), each flagged in analyze.py. Recommend committing raw copybench logs in a follow-up wave.
- **F2-2 (MINOR, source citation)**: analysis.md T1 "Source CSV" column + RFC footnotes ¹–⁴ cite per-cell `timing.csv` files — all 72/72 are header-only (GPU timestamps non-functional, documented analysis.md L73). True provenance of same-device fps = per-cell `run.err` (auditor-verified); cross frame-time/delta columns are model estimates (disclosed). Suggest renaming column or citing run.err.
- **F2-3 (MINOR, wording)**: RFC draft L11 TL;DR "Intel iGPU misses by 2-3 orders of magnitude" — actual per its own tables: 147.06/8.33 = 17.6× (1.25 orders) m2, 125.00/4.17 = 30.0× (1.48 orders) m4. Suggest "over an order of magnitude (18-30×)".
- **F2-4 (INFO)**: F1-A row attribution confirmed clean in committed docs — 11.50 ms only on Intel→9060XT row (analysis.md L28, RFC L19); 9.42 ms only on Intel→9070XT row (analysis.md L68).
- **F2-5 (INFO)**: `feat/dual-gpu-oneway` @ f298ea2 exists — different plan, out of freeze scope.
- **F2-6 (INFO)**: NVIDIA gate verified — RFC §4 claims all resolve to committed `measurements/nvidia/nvidia-findings.md` (GATED/UNTESTED, preconditions ❌ FAIL); no NVIDIA-derived measured numbers anywhere.
- **F2-7 (INFO)**: analysis.md row order is filesystem-dependent (unsorted `Path.iterdir()`, analyze.py L167-183) — cosmetic only; values reproducible.
- **10-number sample**: 7/10 resolve directly to committed `run.err` artifacts (879.27/459.07/310.73/553.47/6.80/417.40 fps all re-verified); 3/10 copybench-derived (7.89/2.21 ms deltas, 0.8–9.4 ms range) arithmetically verified against committed formulas. Checks (b) no TODO/FIXME/XXX PASS, (c) no lsfg-vk-ui/ changes PASS, (e) all 4 frozen SHAs match PASS.
