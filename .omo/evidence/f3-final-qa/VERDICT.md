# F3 FINAL VERIFICATION — dual-GPU (issue #159)

- Date: 2026-08-24
- Reviewer: F3 final QA (independent rerun; no task-17 evidence reused)
- Branch: `feat/dual-gpu` @ `4fdfcdd` ("fix(layer,backend): fresh-per-cycle sync-fd edges and modifier-zero drm path")
- Build: fresh Release tree `/tmp/opencode/build-f3` (cmake -DCMAKE_BUILD_TYPE=Release, 0 errors)
- Runner: `LSFGVK_CLI_BIN=/tmp/opencode/build-f3/lsfg-vk-cli/lsfg-vk-cli LSFGVK_MATRIX_OUT=<this dir> scripts/run-matrix.sh live`
- Validation: VK_LAYER_KHRONOS_validation 1.4.357 active for every matrix pair and the debug run

## F3 VERDICT: APPROVE

## Assertion table (all from THIS run's raw logs)

| # | Assertion | navi48-to-navi48 | intel-to-navi48 | navi48-to-intel | invalid-renderer | Result |
|---|-----------|------------------|-----------------|-----------------|------------------|--------|
| 1 | exit code | 0 | 0 | 0 | 1 (expected nonzero) | PASS |
| 2 | `wait ok` lines = 8×(2−1) | 8/8 | 8/8 | 8/8 | 0 (expected) | PASS |
| 3 | zero `Validation Error\|VUID` stderr lines | 0 | 0 | 0 | 0 | PASS |
| 4 | mode log line | same-device line present, no cross line | cross context line present | cross context line present | named error, no Vulkan work | PASS |
| 5 | cross-gate uuid correlation | n/a | processing uuid `00000000040000000000000000000000` == init-line uuid of Navi48 ≠ game Intel | processing uuid `8680677d060000000002000000000000` == init-line uuid of Intel ≠ game Navi48 | n/a | PASS |

### Cross-pair correlation detail (assertion 5)

- intel-to-navi48 (`per-pair/intel-to-navi48/run.err`):
  - init: `lsfg-vk: processing on 'AMD Radeon RX 9070 XT (RADV GFX1201)' [uuid 00000000040000000000000000000000], dma-buf: yes, drm-modifier-images: yes`
  - ctx:  `lsfg-vk: processing on '00000000040000000000000000000000' (game on 'Intel(R) Graphics (ARL)')`
  - → backend runs framegen on Navi48 while the game renders on Intel. True dual-GPU.
- navi48-to-intel (`per-pair/navi48-to-intel/run.err`):
  - init: `lsfg-vk: processing on 'Intel(R) Graphics (ARL)' [uuid 8680677d060000000002000000000000], dma-buf: yes, drm-modifier-images: yes`
  - ctx:  `lsfg-vk: processing on '8680677d060000000002000000000000' (game on 'AMD Radeon RX 9070 XT (RADV GFX1201)')`
  - → backend runs framegen on Intel while the game renders on Navi48. True dual-GPU, both directions.

### Negative control detail (invalid-renderer)

- `error: failed to find specified GPU: LSFGVK-Matrix-Bogus-GPU` at run.err line 3.
- Zero `lsfg-vk: processing on '` lines in stderr → failure precedes any Vulkan device/backend work.

## Legacy same-device benchmark regression gate

- Command: `lsfg-vk-cli benchmark -d "$DLL" -g "AMD Radeon RX 9070 XT (RADV GFX1201)" -t 3`
- Exit: 0 · iterations/generated frames: 3724/3724 · **fps (generated): 1241.33** — inside the ~1200–1260 baseline band (task-15 baseline ≈1204; noise band per inherited wisdom). No regression.

## Explicit validation-layer debug run (same-device)

- Command: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation lsfg-vk-cli debug -d "$DLL" -g "AMD Radeon RX 9070 XT (RADV GFX1201)" -w 640 -h 360 frames-640x360/`
- Exit: 0 · waits: 8/8 · `Validation Error|VUID` lines: 0 · same-device line present.
- radv "not a conformant Vulkan implementation" warnings present as expected and excluded from the gate per known-noise policy.

## Evidence inventory (this directory)

- `matrix.log` — full runner console incl. per-pair verdicts + `matrix RESULT: PASS (all pairs green)`, runner exit 0
- `per-pair/<pair>/{run.log,run.err}` × 4 pairs
- `benchmark.log` / `benchmark.err`
- `debug-validation.log` / `debug-validation.err`
- `frames-640x360/*.dds` — deterministic generated input frames

## Notes

- Known residual (documented task-14, out of F3 scope): dma-buf extension gate decided once at lazy backend emplace; mixed same-device-first multi-context processes still hit the named reconstruct error. Single-mode processes (as tested here) unaffected.
