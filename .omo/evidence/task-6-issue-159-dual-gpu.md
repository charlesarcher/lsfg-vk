# Task 6 Evidence: Configuration.md dual-gpu semantics rewrite

Plan: .omo/plans/issue-159-dual-gpu.md, todo 6 (Wave 1)
File touched: docs/Configuration.md ONLY

## Rewritten section text (docs/Configuration.md:24-34)

```markdown
- **GPU / `gpu`**: The GPU used for frame generation (the "processing GPU"). If left unset, frames are processed on the game's own GPU instead of an arbitrary first device found, which matches single-GPU behavior exactly. When set to a different GPU, lsfg-vk enters dual-GPU mode: the game keeps rendering and presenting on its own GPU, while the entire frame generation pipeline runs on the selected device. Frames are copied between both GPUs over PCIe, which costs bandwidth and adds latency:

  | Scenario | Approx. PCIe traffic | Link requirement |
  | --- | --- | --- |
  | 1440p SDR, 60 fps, multiplier 2 | ~1.8 GB/s | Any modern PCIe link suffices |
  | 1440p SDR, 240 fps, multiplier 4 | ~14 GB/s | Requires a x8-class link or better |
  | Added latency (Windows community measurements) | n/a | ~3-5 ms |

  Unlike on Windows, presentation always stays on the render GPU: the game's WSI swapchain is bound to its own device and surface, so generated frames must travel back over PCIe (this is the core premise of upstream issue #159). You can identify a GPU through its name (e.g. `NVIDIA GeForce RTX 3080`), uppercase-only ID (e.g. `0x10DE:0x2C02`) or PCI bus ID (e.g. `3:0.0`). Changing this option requires an application restart, because the processing GPU is fixed while the process runs. On NVIDIA, dual-GPU mode is expected to work through the standard Linux dma-buf mechanism (`VK_EXT_external_memory_dma_buf`, supported since driver 515.43.04), but it remains unverified, as no NVIDIA test hardware was available for this project.

The "Multiplier", "Flow Scale" and "Performance Mode" options can be **hot-reloaded**, meaning that changes to these options will take effect immediately without needing to restart the application. Options such as "Pacing Mode" or removal of the profile require a swapchain recreation, which usually means resizing or restarting the application. Any other change requires an application restart. This includes the "GPU" option: changing it only takes effect on the next launch of the application, since the processing GPU cannot be swapped while the process is running.
```

## Requirements coverage

- "Dual GPU is NOT supported" and "MUST be the same GPU": removed entirely.
- Identification syntaxes: unchanged verbatim (name / uppercase-only `vendorID:deviceID` / PCI bus ID).
- New default semantics: unset processes on the game's own GPU, explicitly not "an arbitrary first device found"; identical to previous single-GPU behavior.
- Restart-required for mid-session `gpu` changes: stated in bullet AND extended hot-reload paragraph (:34).
- PCIe cost table numbers match plan EXACTLY: ~1.8 GB/s (1440p SDR m=2), ~14 GB/s (1440p@240 m=4), x8-class link, ~3-5 ms added latency (Windows community measurements).
- Windows-parity divergence: present always stays on render GPU due to WSI swapchain binding to game device + surface; upstream issue #159 cited.
- NVIDIA status note: expected to work via VK_EXT_external_memory_dma_buf (driver >= 515.43.04), unverified on project hardware (no NVIDIA test rig).
- LSFGVK_GPU env var (:62 post-edit): left untouched; it never repeated the same-GPU restriction ("GPU to use for frame generation."), so diff minimality applies.

## Verification

- Grep gate: `grep -ri "dual gpu is not supported\|must be the \*\*same gpu" docs/Configuration.md` returns NOTHING (exit=1). Full output in task-6-issue-159-dual-gpu.grepout.
- Markdown structure intact: same heading hierarchy, option-name-first bold bullets preserved, table indented 2 spaces inside the list item (CommonMark continuation), neighboring bullets byte-identical.
- Diff minimality verified via git diff: only the gpu bullet block + one appended sentence in the hot-reload paragraph changed; all other lines byte-identical.
- Terminology locked for todo 16: "processing GPU", "game's own GPU"/"render GPU", "dual-GPU mode".
