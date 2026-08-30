# Draft: dual-gpu-measurements

> SUPERSEDED-IN-PART by new plan request: one-way dual-GPU ("true dual GPU").
> This draft's measurement campaign becomes a follow-up covering both modes;
> see .omo/drafts/oneway-dualgpu.md for the active plan draft.

> NOTE: scaffold-plan.mjs could not run (no shell tool in this planning session).
> Hand-written draft preserving the script's field contract. Re-run script at
> plan-creation time if available.

## Status
- intent: clear
- review_required: false
- status: exploring -> awaiting-fork-answer

## Request
RFC #550 response work: replace placeholder latency/bandwidth numbers with real
measurements/traces on the local rig, and produce the "Remaining Tasks" plan
addressing PancakeTAS's two concerns (cross-vendor dma-buf inconsistency incl.
NVIDIA breakage; sequential-copy latency at high multipliers / non-8-bit formats).

## Grounded facts
- timestampPeriod=10ns on RADV 9060 XT (repo-root VP_VULKANINFO json); no existing
  vkCmdWriteTimestamp usage anywhere in tree
- Backend probe points: lsfg-vk-backend/src/lsfgvk.cpp scheduleFrames path,
  lsfg-vk-common/src/vulkan/command_buffer.cpp submit()/blitImage()
- CLI: benchmark.cpp measures CPU wall-clock only; debug tool has -r/--render-gpu
- HDR = R16G16B16A16_SFLOAT = 2x copy bytes vs RGBA8
- Layer architecture always pays copy-in AND copy-back (presentation stays on render
  GPU) - unlike Windows dual-GPU guide's "display on secondary" advice
- Windows ~3-5ms figure origin confirmed (community OSLTT measurements) - our number
  must be measured, not cited

## Open forks
1. [ASKED] Where does measurement instrumentation live?
   A (rec): separate measure/timing branch off v10-dual-gpu - keeps PR stream clean,
   mergeable later | B: in-tree opt-in env gate on stream itself | C: uncommitted scratch

## Defaults adopted (no ask)
- Methodology: GPU timestamps (ring-buffered pools, read N frames late, mask to
  timestampValidBits) + fdinfo engine timers cross-check + sysfs PCIe link recording;
  NOT present-to-present external latency capture
- NVIDIA phase included as gated tasks (preconditions: hardware access, driver >=610)
- RFC reply drafted as text artifact; user posts manually (established pattern)
- Optimization work explicitly OUT of scope this round - measure-first, data decides

## Decisions ledger
- D1 (user): instrumentation lives on separate `measure/timing` branch off
  v10-dual-gpu; PR stream stays 10 commits; mergeable later if maintainer wants it

## Approval gate
- APPROVED by user ("write the full plan")
- plan written: .omo/plans/dual-gpu-measurements.md (10 impl + F1/F2)
- status: handed-off
