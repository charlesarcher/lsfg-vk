# task-16 evidence: Troubleshooting dual-GPU section + platform caveats

Date: 2026-08-24
File modified: docs/Troubleshooting.md ONLY (as required; Configuration.md untouched, todo 6 semantics preserved byte-identically).

## What changed

### 1. Superseded the wrong-GPU note (old :30)
Old text documented the first-enumerated hazard ("lsfg-vk might be defaulting to a different one than the game is using"). That hazard is eliminated by todo 10's game-device default picker. New text states:
- wrong/unsatisfiable `gpu` configs now fail with a NAMED error quoting the configured string and the affected device (implementation: lsfg-vk-layer/src/instance.cpp:344-347 "failed to create backend instance for requested gpu '<str>'" wrapping lsfgvk.cpp:224 "no suitable physical device found"; extension-missing case instance.cpp:219-221 names gpu + game device + missing extensions);
- leaving `gpu` unset always works because frame generation pins to the game's own GPU;
- pointer to the new Dual-GPU Setups section.

### 2. New "### Dual-GPU Setups" section (between Performance Overlays and Opening a Bug Report)
Covers all five mandated bullets:

a) **Flatpak both render nodes** (:51): sandbox must see `/dev/dri/renderD*` of BOTH the game GPU and processing GPU; cross-links docs/Flatpak-Guide.md (relative link, README-style). Note: Flatpak-Guide.md itself carries no render-node guidance today (only config-dir/steamapps overrides), so the requirement is stated here and the guide is linked for general setup only.

b) **Log-line reading** (:42-46): documented against the LANDED strings, which differ from the plan's paraphrase:
   - single-GPU: `lsfg-vk: frame generation on the game's own device '<name>'` (swapchain.cpp:151-152) - as planned;
   - dual-GPU: `lsfg-vk: processing on '<uuid32hex>' (game on '<name>')` (swapchain.cpp:148-149) - the FIRST field is the processing device's UUID in lowercase hex, NOT its name (backend::Instance has no device-name accessor; frozen API). Doc explains this and points at the startup line `lsfg-vk: processing on '<device name>' [uuid ...]` (lsfgvk.cpp:434-440) for the name-to-uuid mapping;
   - restart honesty: `lsfg-vk: gpu change requires restart to take effect` (instance.cpp:168).

c) **LINEAR / empty-modifier-intersection implications** (:49-50): both devices must agree on a DRM modifier for exchanged frames; cross-vendor pairs typically share none, so LINEAR dma-bufs are used. States the rig-verified fact that LINEAR accepts every needed usage (sampling, storage, transfer) on the tested hardware while warning that storage-on-LINEAR is driver-dependent elsewhere. Failure surfaces as the named negotiation error (swapchain.cpp:195-197).

d) **NVIDIA status** (:49 tail): mirrors Configuration.md:32 verbatim in substance: mechanism `VK_EXT_external_memory_dma_buf` available since driver 515.43.04, remains unverified, no NVIDIA test hardware was available.

e) **HDR caveat** (:52): cross-device HDR transport ships unverified; SDR-only CLI harness; asks reporters to mention HDR+dual-GPU failures.

f) **Pipeline cache per-driver-UUID** (:53, from todo 11): pattern verbatim `lsfg-vk_pipeline_cache_<driverUUID>.bin` under `$XDG_CACHE_HOME` falling back to `~/.cache`; same-driver GPUs share one file (AMD+AMD verified on rig: shared "AMD-MESA-DRV" driverUUID) while different drivers get separate files (Intel vs AMD verified); legacy unkeyed `lsfg-vk_pipeline_cache.bin` pruned automatically (lsfgvk.cpp:255-257 best-effort remove).

### Terminology consistency
Reuses todo 6's locked terms exactly: "processing GPU", "game's own GPU"/"game's own device", "dual-GPU mode". Intro sentence deliberately echoes Configuration.md:24 phrasing. ASCII hyphens only (checked: no U+2013/U+2014/U+2212 in file).

## Verification (see task-16-issue-159-dual-gpu.grepout)
- Repo-wide stale-claim gate `grep -ri "dual gpu is not supported\|must be the \*\*same gpu" docs/`: EMPTY (exit 1) = PASS.
- Caveat presence greps: flatpak/render node :51, NVIDIA :49, HDR :52, cache pattern :53, log lines :43-46 - all located.
- Cross-doc terminology grep "dual.gpu mode": consistent usage across Configuration.md and Troubleshooting.md.
