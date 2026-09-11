# lsfg-vk Testing, Benchmarking & Diagnostics Tooling Suite

This directory contains the developer and operator tooling for `lsfg-vk`: automated throughput and latency benchmarks, kernel/eBPF sched profilers, dynamic swapchain recreation testbeds, and game launch wrappers.

---

## Directory Taxonomy

```
tools/
├── README.md                      # This manual
│
├── benchmarks/                    # Automated performance & throughput measurement
│   ├── bench_furmark.sh           # Primary A/B doubler benchmark (1440p MSAA4 default; symlinked to repo root)
│   ├── bench_permutations.py      # Automated 12-permutation matrix runner with percentile distributions
│   ├── analyze_logs.py            # Latency log parser & statistical report generator
│   └── campaigns/                 # Multi-GPU campaign validation suites
│       ├── run_matrix.sh
│       ├── run_cross_campaign.sh
│       ├── run_baseline_campaign.sh
│       ├── qa_check_campaign.sh
│       └── run_remaining_cells.sh
│
├── profiling/                     # Low-level eBPF & kernel latency profilers
│   ├── trace_offcpu.sh            # eBPF off-CPU scheduling stall tracer (with optional --ioctls breakdown)
│   ├── trace_ntsync.sh            # Windows NT synchronization event latency tracer (Proton)
│   ├── trace_wake.sh              # Cross-thread scheduler wake latency tracer
│   └── sample_stacks.sh           # Hot-path stack unwinder (eu-stack / gdb batch)
│
├── testing/                       # WSI swapchain robustness & regression testbeds
│   ├── test_vkcube.sh             # Headless fake-swapchain smoke test
│   ├── test_wsi_recreate.sh       # Dynamic swapchain resize/recreation test harness
│   └── wsi_recreate.c             # Standalone Vulkan C program for WSI recreation stress testing
│
└── games/                         # Real-game verification wrappers & Steam launchers
    ├── launch_re2.sh              # Resident Evil 2 one-way dual-GPU Steam launch wrapper
    ├── launch_re2_baseline.sh     # Native unlayered baseline launcher with perf record
    ├── debug_re2.sh               # Standalone Proton test harness (bypasses Steam client)
    ├── debug_re2_overlay.sh       # Steam overlay injection bisector
    └── launch_app.sh              # Standalone launcher for lsfg-vk-app
```

---

## 1. Benchmarking Suite (`tools/benchmarks/`)

### `bench_furmark.sh`
The primary automated A/B benchmark for verifying frame-generation throughput retention and presentation rates without requiring Steam.

- **Usage**:
  ```bash
  ./tools/benchmarks/bench_furmark.sh {baseline|doubled|ab} [seconds] [preset]
  ```
  *(Note: A convenience symlink `./test_furmark.sh` exists at the repository root).*

- **Modes**:
  * `baseline`: Measures unlayered native FurMark on the render GPU.
  * `doubled`: Launches `lsfg-vk-app` on the presentation GPU, injects `lsfg-vk-layer`, and measures doubled output.
  * `ab`: Executes `baseline` followed by `doubled`, reporting native retention % (target: $\ge 95\%$).

- **Presets**:
  * `bound`: `2560x1440 MSAA 4 vsync 0` (Default — GPU-bound realism, ~140–150 native FPS).
  * `light`: `1280x720 MSAA 1 vsync 0` (Lightweight — exposes presentation queue bottlenecks).
  * `panel`: `2560x1440 MSAA 1 vsync 0`.
  * `heavy`: `2560x1440 MSAA 8 vsync 0`.

- **Environment Overrides**:
  * `MANGOHUD=0`: Disables MangoHud for pure numeric headless A/B comparisons.
  * `RENDER_GPU_INDEX=1`: Vulkan device index for rendering (default: 1, e.g. RX 9070 XT).
  * `LSFGVK_PROFILE=furmark-oneway`: Layer profile override from `~/.config/lsfg-vk/conf.toml`.

- **Examples**:
  ```bash
  # Standard 8-second A/B test at 1440p MSAA 4
  ./tools/benchmarks/bench_furmark.sh ab 8 bound

  # Quick 3-second smoke test on the doubled pipeline without overlay
  MANGOHUD=0 ./tools/benchmarks/bench_furmark.sh doubled 3 bound
  ```

---

### `bench_permutations.py`
Automated topology matrix runner that tests native baselines and all directed GPU $\to$ GPU offload pairs. Reports presentation count, FPS, and full statistical distributions ($p_{10}, p_{50}, p_{90}, p_{99}$, mean, std).

- **Usage**:
  ```bash
  python3 tools/benchmarks/bench_permutations.py [--seconds 6] [--preset bound] [--json output.json]
  ```

- **Example**:
  ```bash
  python3 tools/benchmarks/bench_permutations.py --seconds 6 --preset bound --json results.json
  ```

---

## 2. Low-Level Latency Profilers (`tools/profiling/`)

Requires root privileges and `bpftrace` installed.

### `trace_offcpu.sh`
Traces off-CPU time by pairing `raw_syscalls:sys_enter` and `raw_syscalls:sys_exit` on target threads. Identifies where threads are blocking (futex waits, compositor poll/socket waits, or DRM ioctl fence stalls).

- **Usage**:
  ```bash
  sudo ./tools/profiling/trace_offcpu.sh [--ioctls] [seconds] [process_name]
  ```

- **Examples**:
  ```bash
  # Basic syscall census for Resident Evil 2
  sudo ./tools/profiling/trace_offcpu.sh 60 re2.exe

  # Detailed breakdown of DRM ioctl request codes (separating AMDGPU CS submissions)
  sudo ./tools/profiling/trace_offcpu.sh --ioctls 60 re2.exe

  # Trace FurMark off-CPU stalls
  sudo ./tools/profiling/trace_offcpu.sh 30 furmark
  ```

---

### `trace_ntsync.sh`
Measures thread wait durations on Windows NT synchronization objects in Proton/Wine via `ntsync` ioctl `0xc0284e82` (`NTSYNC_IOC_WAIT_ANY`). Flags long waits ($>20\text{ ms}$) that indicate presentation feedback timer gates.

- **Usage**:
  ```bash
  sudo ./tools/profiling/trace_ntsync.sh [seconds] [process_name]
  ```

---

### `trace_wake.sh`
Correlates engine thread wake events (`sched:sched_wakeup`) with long ioctl waits ($>8\text{ ms}$) to identify cross-thread waking latency.

- **Usage**:
  ```bash
  sudo ./tools/profiling/trace_wake.sh [seconds] [process_name]
  ```

---

### `sample_stacks.sh`
Non-invasively attaches to a target game process after a configurable warmup duration and takes stack snapshots across all threads using `eu-stack` or `gdb`.

- **Usage**:
  ```bash
  ./tools/profiling/sample_stacks.sh [process_name] [warmup_seconds] [samples] [interval_seconds]
  ```

- **Example**:
  ```bash
  # Wait 45s for game boot, take 5 stack samples every 8 seconds
  ./tools/profiling/sample_stacks.sh re2.exe 45 5 8
  ```

---

## 3. Testing & WSI Regression Testbeds (`tools/testing/`)

### `test_vkcube.sh`
Exercises `vkcube` under the isolated swapchain capture layer, verifying DMA-BUF export and external presentation handshake without needing a full game title.

- **Usage**:
  ```bash
  ./tools/testing/test_vkcube.sh [seconds]
  ```

---

### `test_wsi_recreate.sh`
Compiles and executes `wsi_recreate.c`, which simulates windowed $\to$ fullscreen transitions and in-flight resolution switches ($1080\text{p} \to 1440\text{p}$) while frame-doubling is active.

- **Verifies**:
  * Isolated swapchain destroys and recreates without `VK_ERROR_DEVICE_LOST`.
  * IPC socket streams disconnect and reconnect smoothly.
  * `lsfg-vk-app` dynamically adapts its presentation textures without memory leaks.

- **Usage**:
  ```bash
  ./tools/testing/test_wsi_recreate.sh
  ```

---

## 4. Game Launchers (`tools/games/`)

### `launch_re2.sh`
Production Steam launch wrapper for Resident Evil 2.

- **Steam Launch Options**:
  ```
  /path/to/lsfg-vk/tools/games/launch_re2.sh %command%
  ```

- **Behavior**:
  1. Spawns `lsfg-vk-app` on the presentation GPU (if not already running).
  2. Binds rendering to the primary GPU (`1002:7550`).
  3. Injects `lsfg-vk-layer` with `MangoHud`.
  4. Automatically terminates `lsfg-vk-app` when the game process exits so exclusive Wayland layer-shell does not hold the display.

---

### `launch_re2_baseline.sh`
Unlayered native baseline launcher that strips all `LSFGVK_*` environment variables and records CPU profile data with `perf record`.

- **Steam Launch Options**:
  ```
  /path/to/lsfg-vk/tools/games/launch_re2_baseline.sh %command%
  ```
