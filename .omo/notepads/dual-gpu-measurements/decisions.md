# Decisions — dual-gpu-measurements

Architectural choices and rationales discovered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## Recovery Report — 2026-08-29

Lost plan work (stash@{0} on `measure/timing` + untracked files in main worktree) recovered into a clean commit stream in worktree `/tmp/opencode/measure-timing`.

**Commit stream (base `c376f8e` + 9 recovered commits):**

| Hash | Commit |
|------|--------|
| `6b737cd` | `feat(measure): opt-in gpu timestamp staging for dual-gpu paths` |
| `20e6e4f` | `feat(cli): surface timing capture in benchmark/debug` |
| `76d2e62` | `feat(cli): copybench cross-device transport microbenchmark` |
| `3089a8a` | `data(measure): same-device baselines` |
| `925b0c4` | `data(measure): cross-device matrix` |
| `f167a3e` | `data(measure): analysis of dual-gpu latency and bandwidth` |
| `56a8388` | `docs: replace placeholder dual-gpu figures with measurements` |
| `c1be639` | `data(measure): nvidia cross-vendor findings` |
| `17bd0c4` | `docs(measure): rfc 550 reply draft` |

**Build gate:** clean Release build (layer + cli), 0 warnings, 0 errors. Log: `/tmp/opencode/r2-build.log`.

**Documented deviation (1):** `copybench.cpp` lines 261/278, `(iter == 0) ? 0 :` → `0u :` — fixes 2 GCC `-Wnarrowing` warnings in the recovered (new, untracked) file required by the zero-warning gate. Semantically identical (VkAccessFlags is unsigned). Folded into `76d2e62` via fixup rebase.

**Smoke tests** (`/tmp/opencode/r2-smoke.log`): usage strings verified (`--timing-csv`, `--hdr`, `--render-gpu` present in benchmark/copybench/debug); 10 s gate-off benchmark on 9070XT 1080p SDR m2 with the real Lossless.dll: exit 0, 12 427 frames, ~1243 fps; `timing.csv` header-only, consistent with the committed baseline cells (single-GPU mode).

**Final verification (all passed):**
1. Recovery worktree clean (`git status --porcelain` empty).
2. Both stashes intact and byte-identical to the pre-recovery snapshot (`stash@{0}` measure/timing WIP, `stash@{1}` oneway-dualgpu).
3. Main worktree `git status` byte-identical to the pre-recovery snapshot.
4. `git diff c376f8e..HEAD` over the 17 tracked code files == `git stash show -p stash@{0}` — 0 lines of deviation (the `0u` fix is in untracked `copybench.cpp`, not part of the tracked diff).
5. All 19 recovered code files byte-identical to the pre-recovery backup (`sha256sum -c /tmp/opencode/r1-final.sha256`).

**Artifacts (in `/tmp/opencode/`):** `r1-final/` + `r1-final.sha256` (code backup), `r1-stashed-code.diff`, `r0-stash-before.txt`, `r0-mainworktree-status-before.txt`, `r2-cmake-configure.log`, `r2-build.log`, `r2-smoke.log`, `r2-smoke-bench.csv`.

Not pushed, not tagged, no PR — per plan constraints.

## R0 Verification Sign-off — 2026-08-29 (orchestrator)

Independent verification of the R0 recovery — ALL PASSED:
- 10 commits `c376f8e..17bd0c4` with exact plan messages; worktree `/tmp/opencode/measure-timing` clean; refs: measure/timing=17bd0c4, v10-dual-gpu=ed79315, feat/dual-gpu=1b8d2fd, feat/dual-gpu-oneway=f298ea2; both stashes intact; main worktree status byte-identical (MAIN-STATUS-UNCHANGED).
- Code diff vs `stash@{0}`: tracked-file portion byte-identical; sole delta = the 4 new (previously untracked) files (copybench.cpp/hpp, timestamps.hpp/cpp) — expected, stash did not include untracked files.
- Scope: branch diff touches only {docs, lsfg-vk-backend, lsfg-vk-cli, lsfg-vk-common, lsfg-vk-layer, measurements} (1035 files, +9172/−25); zero `lsfg-vk-ui/` changes; campaign scripts under `measurements/scripts/`.
- Documented `0u` deviation confirmed WORKTREE-LOCAL: committed `copybench.cpp` has `0u` (lines 261/278); main worktree untracked copy still has original `0` → main worktree unmodified.
- Build re-verified: 0 warnings (`grep -ci warning r2-build.log` = 0); smoke re-run: `--timing-csv` in benchmark usage, `--render-gpu` in copybench usage, BENCH_EXIT=0.
- F1/F2 reference points: committed cross cell `Intel-9070XT/1440p-SDR-m2` records NO fps (empty timing summary in run.err, header-only timing.csv); gate-off fps reference = `raw/baseline/9070XT/1440p-SDR-m2/run.err` → 879.27 gen-fps / 1758.57 total; cross frame-time model for the cell = 11.50 ms (3.22 same-device + 8.28 copy, per analysis.md Table 1).
- F2 known risk areas: analysis.md cites uncommitted `learnings.md` as copybench source; Table 1 "Source CSV" column points at header-only CSVs while values derive from run.err/copybench CPU timings; task 9 NVIDIA numbers must resolve to gated-status statements in nvidia-findings.md.
