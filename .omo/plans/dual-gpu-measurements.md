# Plan: dual-gpu-measurements

```yaml
slug: dual-gpu-measurements
created: 2026-08-25
intent: clear
review_required: false
approved_by_user: yes (this session)
branch_strategy: all work on NEW branch measure/timing cut from v10-dual-gpu
executor_contract: decision-complete; zero interview context assumed
```

## Goal

Replace every placeholder performance number in the dual-GPU story with rig-measured
data, and produce the artifacts needed to answer RFC #550
(https://github.com/PancakeTAS/lsfg-vk/discussions/550) with evidence:

1. Measured per-stage GPU latency breakdown of the dual-GPU exchange path
   (replaces the cited "~3–5 ms" Windows-community placeholder).
2. Measured achieved PCIe bandwidth of the dma-buf exchange (validates/replaces the
   theoretical "~1.8 GB/s @1440p60×2 … ~14 GB/s @240×4" model).
3. Copy-transport isolation microbenchmark (raw cross-GPU copy rates, per pair,
   resolution, and format incl. non-8-bit — PancakeTAS concern #2).
4. Gated NVIDIA phase producing precise failure-mode documentation (concern #1),
   using the weekend 9070 XT + RTX 5090 machine.
5. Updated docs + ready-to-post RFC reply draft (user posts manually).

All instrumentation lives on `measure/timing`; the 10-commit PR stream
(`v10-dual-gpu` / fork `feat/dual-gpu`) stays untouched (user decision D1).

## Background context (worker briefing — you were not interviewed; this is your ground truth)

### Repository & branch state

- Repo root: `/home/archerc/code/lsfg-vk`. Resting checkout: `feat/dual-gpu`
  (23-commit original history + 20/20 plan record). Do NOT touch it except to read.
- `v10-dual-gpu` = the 10-commit kernel-style stream (HEAD `ed79315`,
  "docs: dual-GPU setup guide…"), byte-identical product tree to the tested state,
  tracks `PancakeTAS/lsfg-vk:develop`.
- Fork remote name: `fork` → `github.com:charlesarcher/lsfg-vk.git`, branch
  `feat/dual-gpu` currently at `ed79315`. Push policy: **force-with-lease only**.
- **NO PR may be created. No GitHub discussion comment may be posted by the agent.**
  gh CLI appears logged-out but API + SSH work; user is `charlesarcher`.
- Build: existing green `build/` tree; rebuild with your usual cmake invocation;
  0-warning baseline must hold (`build/` currently compiles clean).
- Delegation-plane health unknown: fire one trivial canary task first; on provider
  failure proceed with direct execution and record the deviation in
  `.omo/start-work/ledger.jsonl` (established precedent from prior plan).
- FIRST ACTION: `git checkout -b measure/timing v10-dual-gpu` — every subsequent
  commit lands there. Never commit on any other branch.

### Rig ground truth (exact strings)

| # | vulkaninfo order | Exact deviceName | DRM card | Notes |
|---|---|---|---|---|
| 0 | GPU0 | `AMD Radeon RX 9060 XT (RADV GFX1200)` | card3 | vendorID:deviceID `0x1002:0x7590` |
| 1 | GPU1 | `AMD Radeon RX 9070 XT (RADV GFX1201)` | card2 | `0x1002:0x7550` |
| 2 | GPU2 | `Intel(R) Graphics (ARL)` | card1 | Arrow Lake iGPU |

- Lossless.dll: `/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll`
- Layer live-test rig: `/tmp/opencode/layer-test/{layer_json.json,liblsfg-vk-layer.so}`
  (json carries ABSOLUTE library_path); run env:
  `VK_LAYER_PATH=/tmp/opencode/layer-test VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation`
- `vkcube --gpu_number {0=9060XT, 1=9070XT, 2=Intel} --present_mode fifo`
- Debug-frame assets: `/tmp/opencode/t5-frames` (4× DDS 640×360 → always pass
  `-w 640 -h 360` to the debug tool)
- Config-file resolution order (source: `config.cpp::findConfigurationFile`):
  `LSFGVK_CONFIG` → `$XDG_CONFIG_HOME/lsfg-vk/conf.toml` → `$HOME/.config/lsfg-vk/conf.toml`
  → `/etc/lsfg-vk/conf.toml`. **In opencode shells `XDG_CONFIG_HOME` is redirected**
  (observed: `/home/archerc/.cache/opencode/config.*/`), and a default-generated
  config whose shipped profiles match `vkcube` may shadow edits — prefer
  `LSFGVK_ENV=1 LSFGVK_GPU=<name> LSFGVK_DLL_PATH=<path>` env-var mode for all
  measurement runs (bypasses conf entirely, already proven).
- Filter radv validation noise: `grep -E 'Validation Error|VUID'` on stderr.
- Success log line shape: `lsfg-vk: processing on '<uuid>' (game on '<deviceName>')`.

### What RFC #550 established (the questions this plan answers)

PancakeTAS (maintainer):
- Prior art existed (`test/dualgpu`, deleted). AMD↔AMD worked; AMD↔Intel had issues;
  **NVIDIA↔anything failed without GBM allocation** ("incredibly slow"); he has
  unmerged DMA-BUF-less copy code in Discord.
- Latency matters more than bandwidth on ordinary links; copies happen **in sequence,
  not simultaneously** → claims 1080p 60→240 is "completely atrocious"; typical
  dual-GPU users sit on old non-bifurcation boards. Scrapped his approach; building a
  "true dual GPU implementation" (his answer to "what is that?" is still pending —
  do NOT speculate about it in any artifact).
charlesarcher (user) already replied: Mesa passes all 9 pairs locally; NVIDIA
unverified; cost model figures were theoretical; **~3–5 ms latency is a Windows
community citation, not measured here** — this plan replaces it.

### Architecture facts the instrumentation relies on

- Backend API: `lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp` —
  `Instance::openContext(descriptor overload)`, `Context::scheduleFrames(...)`;
  cross-device mode uses SYNC_FD binary-semaphore handshakes replicating the timeline
  choreography (see header docs + `scheduleFrames`).
- Per-frame GPU command recording lives in `lsfg-vk-backend/src/lsfgvk.cpp`
  (shader chains: `src/shaderchains/{mipmaps,alpha0,alpha1,beta0,beta1,delta0,delta1,gamma0,gamma1,generate}.cpp`),
  issued through `lsfg-vk-common/src/vulkan/command_buffer.cpp`
  (`CommandBuffer::{begin,dispatch,blitImage,copyBufferToImage,end,submit}`).
- Copy topology per displayed frame (dual-GPU mode):
  1. **game device**: present-hook copy of the game's swapchain image into the
     exported source dma-buf (recorded in the LAYER, `lsfg-vk-layer/`)
  2. **processing device**: flow-field + generation chain reads imported sources,
     writes exported dest dma-bufs
  3. **game device**: copy of imported dest into the real swapchain image
     (also in the LAYER)
  There is NO display-on-secondary shortcut in layer architecture: both directions
  are always paid. Measure them separately.
- Formats: SDR = `VK_FORMAT_R8G8B8A8_UNORM`; HDR = `VK_FORMAT_R16G16B16A16_SFLOAT`
  (= 2× copy bytes — the maintainer's non-8-bit concern).
- Timestamps: RADV `timestampPeriod=10` ns confirmed
  (`VP_VULKANINFO_AMD_Radeon_RX_9060_XT_(RADV_GFX1200)_26_2_1.json` in repo root);
  ANV unconfirmed (task 1 settles it). No `vkCmdWriteTimestamp` usage exists yet.
- fdinfo per-engine busy counters (`drm-engine-*` under `/proc/<pid>/fdinfo/*`)
  provide the bandwidth cross-check; sysfs `current_link_speed`/`current_link_width`
  under `/sys/class/drm/card{1,2,3}/device/` provide the link ceiling context.

## Scope

### In scope
Everything below on `measure/timing`: additive timestamp instrumentation (env-gated),
CLI timing plumbing, `copybench` subcommand, measurement campaign (baselines +
cross-device matrix + analysis), measured-number docs update, gated NVIDIA phase,
RFC-reply draft text, raw-data + analysis artifacts committed under `measurements/`.

### Out of scope — Must-NOT-Have
- ANY change to branches `v10-dual-gpu`, `feat/dual-gpu` (local or fork), or any
  other existing branch. New branch only.
- Optimization/pipelining engineering (overlapping copies, double-buffering, etc.).
  Measure-first round; data decides later.
- Creating a PR, posting comments/issues/discussions, or any other public GitHub
  action. Drafts only.
- Removing or refactoring existing product code beyond additive probe hooks.
- Any speculative commentary about the maintainer's "true dual GPU implementation".
- Placeholder numbers surviving in updated doc sections (old citations may remain
  only where explicitly labeled as external Windows data).

## Methodology constraints (BINDING for all measurement tasks)

1. **Never compare, add, or subtract GPU timestamps across devices** — each device
   has an independent clock domain. All reported latencies are per-device durations
   (delta within one device's query pool). Cross-device "pipeline latency" is derived
   from CPU-side fence/semaphore waits around `scheduleFrames` call sites, labeled as
   such.
2. Query pools are ring-buffered, read back ≥4 frames behind production (never the
   current frame — avoids WAIT stalls), results masked to `timestampValidBits` and
   scaled by `timestampPeriod`. Reset before reuse.
3. Every cell: discard ≥500 warmup frames; sample ≥3000 generated frames; report
   p50/p95/p99/max. Record GPU clocks (fdinfo) and PCIe link state during the run.
4. Primary data source = deterministic CLI loads (`benchmark`, `debug`, `copybench`).
   Live `vkcube` is used for spot sanity checks only (fifo mode).
5. When `LSFGVK_TIMING` is unset the binary must create zero query pools and take
   zero extra barriers — gate-off ≙ bit-identical behavior (verified in F1).
6. Raw data lands under `measurements/raw/<cell>/` and is COMMITTED; analysis derives
   exclusively from committed raw data (traceability requirement, see F2).

## Todos

- [x] 1. Pre-flight rig capability & topology record
    - Recommended task executor category: quick
    - References: `vulkaninfo --summary`; `/sys/class/drm/card*/device/{current_link_speed,current_link_width}`;
      repo-root `VP_VULKANINFO_*.json` (RADV reference); `vulkaninfo --json=<n>` limits dump.
    - Task: For each of the 3 GPUs record into `measurements/rig-capabilities.md`:
      `timestampPeriod`, `timestampValidBits` of the graphics+compute queue families,
      `timestampComputeAndGraphics`, driver version (`driverInfo`), PCIe link
      speed/width per DRM card, and Mesa version. Convert ticks→ns factors into a
      table the later tasks cite.
    - Acceptance: `measurements/rig-capabilities.md` exists with all three GPUs'
      timestamp support confirmed (>0 period, >0 valid bits) or an explicit
      fallback note; link table complete (speed + width for card1/card2/card3);
      values match a fresh `vulkaninfo` rerun.
    - QA: re-run `vulkaninfo --json=<n>` for each GPU and diff key fields against the
      committed table. Evidence: `measurements/rig-capabilities.md` + terminal log
      excerpt appended under `measurements/raw/_preflight.log`.
    - Contingency: if ANV lacked timestamps (not expected), Intel-side stage timing
      falls back to fdinfo-only deltas — record the decision in the md and continue;
      do NOT block the campaign.
    - Commit: `chore(measure): record rig timestamp/link capabilities`

- [x] 2. Env-gated GPU timestamp instrumentation (ring pools + stage probes)
    - Recommended task executor category: deep
    - References: `lsfg-vk-common/include/lsfg-vk-common/vulkan/` (add
      `timestamps.hpp` + `src/vulkan/timestamps.cpp`); probe insertion points:
      `lsfg-vk-backend/src/lsfgvk.cpp` (per-frame command recording between
      shader-chain dispatches: after source-ready barrier → after mipmap chain →
      after alpha/beta/delta/gamma chains → after `generate` → after dest write),
      and the layer's present-hook copy regions in `lsfg-vk-layer/` (game-side
      copy-in and copy-back each get one start/end timestamp pair). Style anchors:
      existing `CommandBuffer` wrappers, clang-tidy-clean, GPL-3.0 headers.
    - Task: Implement `TimingRing` (query pool ring, depth ≥8 frames, 64-bit
      readback masked to `timestampValidBits`, ns conversion via `timestampPeriod`,
      reset-on-reuse, host readback of frame N−4 while N is recorded). Activate ONLY
      when `LSFGVK_TIMING=1`; optional `LSFGVK_TIMING_CSV=<path>` writes one CSV row
      per generated frame:
      `frame_idx,stage_name…` columns `t_copyin_ns, t_flow_ns, t_generate_ns,
      t_copyout_ns, t_total_ns` (processing device) and
      `t_gameside_in_ns, t_gameside_out_ns` (game device rows emitted separately
      with a `side=` column). Insert `vkCmdWriteTimestamp` pairs at the boundaries
      above in BOTH same-device and cross-device paths. Unset gate ⇒ zero pools, no
      behavior change.
    - Acceptance: with gate on, `debug` tool run (640×360, t5-frames, Intel→9070XT)
      produces CSV with ≥ expected frame count and monotonic per-stage durations >
      0; with gate off, `benchmark` gen-fps within ±2% of pre-change baseline and
      `git diff` shows no unconditional Vulkan calls added outside the gate.
    - QA: (a) gate-off A/B benchmark before committing instrumentation (capture
      baseline numbers first!); (b) gate-on CSV sanity: p99 total < 100 ms, no
      negative durations; (c) validation layers clean (`grep VUID`). Evidence:
      `measurements/raw/_instr-smoke/` (CSV + logs). If the delegation plane works,
      execute via one `deep` task; otherwise direct with ledger deviation note.
    - Commit: `feat(measure): opt-in gpu timestamp staging for dual-gpu paths`

- [x] 3. CLI plumbing for timing capture
    - Recommended task executor category: quick
    - References: `lsfg-vk-cli/src/tools/benchmark.cpp`, `lsfg-vk-cli/src/main.cpp`
      (usage text), `lsfg-vk-cli/src/tools/debug.cpp`.
    - Task: Add `--timing-csv <PATH>` to `benchmark` (pass-through of the env gate,
      plus sets it internally so users needn't know the env var) and print a
      per-stage p50/p95 summary at benchmark end; `debug` prints the same summary to
      stderr at exit when the gate is on. Extend usage() text accordingly.
    - Acceptance: `lsfg-vk-cli benchmark --help` shows the flag; a 5-second
      `--timing-csv /tmp/x.csv` run yields the file + printed percentiles consistent
      with the CSV (spot-check p50 total).
    - QA: run once per tool, diff summary vs CSV-derived stats (allow rounding).
      Evidence: `measurements/raw/_cli-smoke.log`.
    - Commit: `feat(cli): surface timing capture in benchmark/debug`

- [x] 4. `copybench` subcommand — isolated dma-buf transport microbenchmark
    - Recommended task executor category: deep
    - References: reuse `lsfg-vk-common/vulkan/exchange.hpp`
      (`exchangeCaps`, `negotiateExchangeLayout`, descriptor struct),
      `image.hpp` export/import helpers, `main.cpp` subcommand pattern,
      `lsfg-vk-cli/src/tools/benchmark.cpp` structure as template.
    - Task: New `copybench` subcommand: args `--render-gpu <name>` + standard
      `-g/-w/-h/--hdr/--iters`; creates an exportable image on the render device and
      an importable image on the processing device with negotiated layout, exports/
      imports via dma-buf descriptors, then loops `--iters` (default 2000)
      `vkCmdCopyImage` submissions with a TimingRing around each copy; prints
      per-iteration ns → GB/s table (bytes = w·h·bytes-per-px) and summary
      percentiles; exits non-zero with the named error when negotiation or import
      fails (this IS the NVIDIA probe surface).
    - Acceptance: runs clean for all 6 ordered pairs of the rig at 1920×1080 and
      2560×1440 in both formats; achieved-GB/s numbers plausible against the link
      ceiling from task 1 (≤ theoretical, ≥ ~0.3× theoretical for AMD pairs);
      validation-clean.
    - QA: cross-check one cell against fdinfo copy-engine busy delta over the run
      (within ~20%); verify a deliberately wrong `--render-gpu` errors with the
      negotiated-layout message. Evidence: `measurements/raw/_copybench-smoke/`.
    - Commit: `feat(cli): copybench cross-device transport microbenchmark`

- [x] 5. Same-device baseline campaign
    - Recommended task executor category: unspecified-low
    - References: task 3 tooling; cells defined below; `measurements/raw/` layout.
    - Task: Run and commit baselines: 3 GPUs (each as the sole/processing GPU,
      game-side = same device) × {1080p, 1440p} × {SDR, HDR} × multiplier {2, 4}
      via `benchmark --timing-csv`, ≥3000 sampled frames each (raise `--duration`
      accordingly). Layout: `measurements/raw/baseline/<gpu-short>/<res>-<fmt>-m<n>/`.
      Each cell dir holds: CSV, tool stdout/stderr log, fdinfo snapshot before/after,
      a `cell.json` (device names, dll hash, mesa version, commit sha).
    - Acceptance: 24 cell dirs (3×2×2×2) each containing all five artifacts; every
      CSV ≥3000 rows post-warmup.
    - QA: scripted check (bash ok) asserting artifact presence + row counts;
      append results to `measurements/raw/_campaign-check.log`. Evidence: the dirs
      themselves.
    - Commit: `data(measure): same-device baselines`

- [x] 6. Cross-device matrix campaign
    - Recommended task executor category: unspecified-low
    - References: tasks 2/3 tooling; pairs below; same layout conventions.
    - Task: Run and commit all 6 ordered pairs — {Intel, 9060XT, 9070XT} game-side ×
      the other two as processing GPUs — × {1080p, 1440p} × {SDR, HDR} × m{2,4}
      (72 cells) via `debug`-tool runs driven by `LSFGVK_ENV=1 LSFGVK_GPU=<proc>`
      with `-r <gamegpu>`; PLUS 12 live `vkcube` spot-checks (Intel-game × both AMD
      processors × {SDR m2} at 640×360 internal, fifo) capturing the cross-device
      log line as sanity evidence. Cell dirs mirror task 5 + `spotcheck/` subdir
      with vkcube logs.
    - Acceptance: 72 complete cells + 12 spot-check logs; each cross-device cell's
      log shows `processing on '<uuid>' (game on '<other>')`; zero VUID errors.
    - QA: same scripted completeness check extended to pairs; verify no cell reused
      another's CSV (row-count uniqueness). Evidence:
      `measurements/raw/_campaign-check.log` (appended).
    - Commit: `data(measure): cross-device matrix`

- [x] 7. Analysis: latency deltas, bandwidth, serialization headroom
    - Recommended task executor category: unspecified-high
    - References: `measurements/raw/**` (committed data only); python3 via stdlib or
      numpy (uv-run acceptable); NO pandas.
    - Task: Write `measurements/analyze.py` + generate `measurements/analysis.md`
      computing per cell: per-stage p50/p95/p99; same-device-vs-cross-device delta
      per matched axis (THE headline number replacing ~3–5 ms); achieved GB/s per
      copy stage (bytes/time) vs sysfs link ceiling; SDR-vs-HDR penalty ratio;
      serialization analysis at the target case (1440p SDR m2, 60→120: per displayed
      frame sum game-in + proc-chain + game-out vs the 8.33 ms budget, and the
      m4/240 Hz variant vs 4.17 ms) stating explicitly whether sequential copies
      fit, and quantifying remaining headroom; gen-fps comparison vs prior
      benchmark band (1200–1350 Navi48 / 878 9060XT). Every number in analysis.md
      must carry a relative path to its source CSV.
    - Acceptance: analysis.md contains: methodology note (incl. the cross-clock-
      domain prohibition), 3 tables (latency deltas, bandwidth, headroom), SDR/HDR
      penalty paragraph, and a "placeholder numbers replaced" checklist mapping old
      figure → new figure → source file.
    - QA: recompute 3 randomly chosen numbers by hand from their CSVs and diff.
      Evidence: `measurements/raw/_analysis-audit.md` with the recomputations.
    - Commit: `data(measure): analysis of dual-gpu latency and bandwidth`

- [x] 8. Docs update with measured numbers
    - Recommended task executor category: quick
    - References: `docs/Dual-GPU-Guide.md` (cost-model section),
      `docs/Configuration.md` (gpu bullet), `docs/Troubleshooting.md`
      (Dual-GPU Setups intro), source: `measurements/analysis.md`.
    - Task: Replace placeholder/theoretical figures with measured ones (keep the
      Windows ~3–5 ms citation ONLY where explicitly labeled "Windows community
      data"), add a short "Measured on this rig" table + link to
      `measurements/analysis.md`. House style: imperative sentences, honest caveats.
    - Acceptance: `grep -nE '3[–-]5 ?ms|~14 ?GB|1\.8 ?GB' docs/` shows only labeled
      external citations; no doc number contradicts analysis.md.
    - QA: grep audit + human-readable diff review. Evidence: commit diff itself.
    - Commit: `docs: replace placeholder dual-gpu figures with measurements`

- [x] 9. NVIDIA phase (externally gated — weekend hardware)
    - Recommended task executor category: deep
    - Preconditions (ALL required before starting, else mark `[~]` with reason in
      this file): RTX 5090 + 9070 XT machine physically accessible; proprietary
      driver installed and version RECORDED (note whether ≥610 series, the mmap-for-
      exported-dma-bufs improvement cited in the RFC thread); Lossless.dll reachable
      on that machine (or mount the known path); build toolchain present.
    - Task: On that machine: (a) record vulkaninfo (extension presence for
      `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`,
      `EXTERNAL_MEMORY_FEATURE_IMPORTABLE` bits for dma-buf handle type);
      (b) attempt `copybench` in all relevant directions (5090↔9070XT, both orders,
      1080p SDR first); (c) on failure capture EXACT error strings + vulkaninfo json
      + driver version into `measurements/nvidia/`; (d) on success escalate to a
      reduced live matrix (both orders × 1080p/1440p × SDR × m2) with the standard
      cell artifacts. Produce `measurements/nvidia/nvidia-findings.md`: per-step
      outcome table, failure taxonomy mapped to the maintainer's claims (import
      rejection vs modifier limits vs GBM-slowness), and whether the 610-series mmap
      change altered behavior.
    - Acceptance: findings md exists with a definitive per-direction verdict
      (works / fails-at-stage-X-with-error-Y / untested-reason) for 5090→9070XT and
      9070XT→5090; all raw captures committed.
    - QA: every claim in findings md traceable to a committed log/json in
      `measurements/nvidia/`. Evidence: those files.
    - Commit: `data(measure): nvidia cross-vendor findings`

- [x] 10. RFC #550 reply draft
    - Recommended task executor category: writing
    - References: `measurements/analysis.md`, `measurements/nvidia/nvidia-findings.md`
      (or its absence), RFC thread content summarized above, user's existing reply
      tone (first person, candid about AI-assisted workflow, offers hardware help).
    - Task: Write `measurements/rfc550-reply-draft.md` — ready-to-post text covering:
      measured latency breakdown (headline: cross-device delta at 1440p SDR m2 vs
      same-device), measured bandwidth vs link ceilings incl. SDR/HDR penalty,
      sequential-copy headroom verdict at the realistic target (1440p60→120) and at
      the maintainer's stress case (1080p 60→240) with honest numbers either way,
      NVIDIA findings (or precise test plan if phase 9 was blocked), explicit
      non-speculation about his "true dual GPU" direction + renewed offer of rig
      access/data. Markdown, GitHub-discussion-appropriate (tables render).
    - Acceptance: draft ≤600 words body + tables; every number footnoted to
      analysis/findings paths; no PR/discussion action taken by the agent.
    - QA: proofread pass against analysis.md numbers (zero mismatch tolerance);
      word count check. Evidence: draft file + audit note appended to
      `measurements/raw/_analysis-audit.md`.
    - Commit: `docs(measure): rfc 550 reply draft`

## Final verification wave

- [x] F1. Reproducibility + gate-off purity audit
    - Recommended task executor category: unspecified-high
    - Task: Fresh checkout of `measure/timing` HEAD into a scratch worktree; rebuild;
      re-run ONE representative cell end-to-end (Intel→9070XT, 1440p SDR m2, debug
      tool, 3000 frames) and compare its per-stage p50s against the committed cell
      (tolerance ±15% per stage, gen-fps ±10%). Separately prove gate-off purity:
      `LSFGVK_TIMING` unset benchmark run within ±2% of the pre-instrumentation
      baseline recorded in task 2's evidence, AND `strace -e ioctl` count delta of
      drm ioctls per frame ≤ +2% vs baseline (no hidden query traffic). Verify
      `git log v10-dual-gpu..measure/timing` contains ONLY this plan's commits and
      `git diff v10-dual-gpu..measure/timing --stat` touches no file outside
      {lsfg-vk-common, lsfg-vk-backend probes, lsfg-vk-cli, lsfg-vk-layer probes,
      docs, measurements}.
    - Acceptance: all tolerances met; branch isolation confirmed; write
      `measurements/_final-f1.md` with the comparison tables.
- [x] F2. Traceability + scope audit
    - Recommended task executor category: unspecified-high
    - Task: Audit that (a) every number in analysis.md, updated docs, and the RFC
      draft resolves to a committed raw artifact (sample 10, resolve all 10);
      (b) `grep -rnE 'TODO|FIXME|XXX'` on the branch diff introduces none;
      (c) no file under `lsfg-vk-ui/` changed; (d) the RFC draft contains zero
      speculation about the maintainer's implementation and zero commitments on the
      user's behalf beyond offering data/access; (e) no branch other than
      `measure/timing` advanced (compare `git for-each-ref` shas against the values
      recorded at plan start: v10-dual-gpu=ed79315, feat/dual-gpu local=1b8d2fd,
      fork feat/dual-gpu=ed79315).
    - Acceptance: written verdict in `measurements/_final-f2.md`; any failure loops
      back to the offending task before handoff.

## Dependency matrix

```
1 ──► 2 ──► 3 ──► 5 ──► 6 ──► 7 ──► 8 ──► 10
│     ╰────► 4 ──────┘ (4 parallel to 3/5, needs 1; feeds 6 spot-checks + 9)
│                ╰────────────────────────► 9 (gated; needs 4 + machine access; blocks only its own findings)
F1 ◄── all impl tasks │ F2 ◄── 7,8,10 (+F1 clean)
```

## Risks & fallbacks

- ANV timestamps missing → fdinfo-only Intel side (decided in task 1; documented).
- Delegation plane down → direct execution + ledger deviation (precedent exists).
- Cross-device deltas noisier than ±15% in F1 → increase sample to 10000 frames for
  that cell and recompute; if still unstable, widen tolerance and SAY SO in analysis
  (never silently relax).
- NVIDIA machine unavailable → task 9 marked `[~]` citing the missing precondition;
  RFC draft ships with the precise test protocol instead of results.
- copybench import fails on a MESA pair (unexpected) → that IS data: capture, file
  under findings, exclude the pair from matrix cells that depend on it and note why.
