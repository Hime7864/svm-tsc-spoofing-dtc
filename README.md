# SVM TSC Spoofing Detection

This project is a research driver designed to detect VMMs that shadow SVME and spoof performance-monitoring timers in order to hide active SVM virtualization.
The detector combines a basic EFER read-overhead check with timing correlation checks that target the subtle missing time introduced by the compensation logic VMMs apply to guest-visible clocks.
It works by correlating two hard-to-spoof, highly consistent time sources:

- `MSR_L3_PACKAGE_ENERGY_STATUS` — a read-only MSR that updates at a steady ~10-15 ms cadence, used to gate the probe window (CPPC-capable CPUs).
- `I/O APIC timer` — an independent hardware timer driven by its own crystal oscillator, used both as a ground-truth reference and, on legacy CPUs without CPPC, as the probe gate.

These sources serve as ground truth for elapsed time. Using them, the expected cycle count can be calculated and compared against the P0-derived timings to quantify any desyncs introduced by time negation in the guest.

**Requirements:** AMD CPU with CPPC support preferred. CPUs without CPPC fall back to a legacy probe path that gates on the I/O APIC timer instead of the energy MSR.

## Project layout

| Path | Role |
|------|------|
| `main.cpp` | Detection logic (`PERFORMANCE_DATA`, `SANITY_DATA`, `DriverEntry`) |
| `llvm-kmd.sln` / `llvm-kmd.vcxproj` | Visual Studio solution — builds an x64 `.sys` driver with ClangCL |
| `Intrinsics/` | Driver runtime and bootstrap |

### Intrinsics runtime

The driver does not link against the WDK import libraries directly. Instead it resolves kernel exports at runtime and boots through a custom entry point:

- `driver_boot.cpp` — entry at `start()`: resolves imports, attaches to the system process, calls `DriverEntry()`, then wipes and unloads the driver image via `CleanupDriver()`.
- `import_resolve.cpp` — hash-based export resolution from `ntoskrnl.exe`, plus signature scans for undocumented routines (`MmPfnDatabase`, `MiGetSystemRegionType`, etc.).
- `msr.cpp` / `cpuid.cpp` — typed wrappers for AMD MSRs and CPUID leaves.
- `utils_*.cpp` — PE parsing, signature scanning, paging helpers, and self-location.
- `fwa.cpp` / `dtlb_*.cpp` — firmware-range page allocator and hosted page-table support used by the bootstrap layer.

Detection code lives in `main.cpp`; the Intrinsics layer handles how the driver loads, resolves its dependencies, and tears itself down.

## Architecture

The detection logic lives in two structures in `main.cpp`:

- `PERFORMANCE_DATA` — snapshots and diffs all timing sources (`read_tsc()`, `diff_tsc()`, overhead measurement). Also records `CurPstate` at probe boundaries.
- `SANITY_DATA` — runs the probe, computes desync ratios, and prints results via `log_results()`.

Boot flow:

1. `start()` resolves kernel imports and signature-scanned symbols.
2. The driver attaches to `PsInitialSystemProcess()` and calls `DriverEntry()`.
3. `DriverEntry()` verifies an AMD CPU, checks CPPC support, allocates a `SANITY_DATA` instance, raises IRQL to `DISPATCH_LEVEL`, and runs either `Run()` (CPPC) or `Run_Legacy()` (no CPPC).
4. Results are logged, the buffer is freed, and `CleanupDriver()` zeroes the mapped image and terminates the loader thread.

## Reported checks

The detector reports five flagged checks plus two informational lines:

- `SVME state` (informational): after the probe, reads `EFER.SVME` in a loop (`pm_counter / 2` iterations on the CPPC path, once on legacy) and prints `ON` or `OFF`. Not counted toward the flagged total.
- `PM Counter` (informational): prints how many EFER reads completed during the gated probe window, plus a dynamic expected count derived from the measured TSC desync ratio. Not counted toward the flagged total.
- `EFER read average`: reads EFER 1000 times at elevated IRQL and measures the average RDTSC delta around each read. Flags when the average exceeds **800** cycles.
- `Power state elevation`: after the probe, forces P0 via CPPC max performance and `PSTATE_CONTROL` (`PstateCmd = 0`), then flags when `CurPstate` matches the commanded P-state. Skipped on the legacy path (always reports OK).
- `TSC desynchronization`: compares writable APERF/MPERF against their read-only counterparts, then compares MPERF against MSR TSC, RDTSC, and RDTSCP. On the CPPC path the combined ratio is normalized per EFER read (`/ pm_counter`). Flags when the average ratio drift exceeds **2.5%**.
- `Interval desynchronization`: normalizes probe deltas with the I/O APIC timer and compares the measured interval against the expected P0 frequency across MPERF, MPERF read-only, MSR TSC, RDTSC, and RDTSCP. On the CPPC path the ratio is normalized per EFER read. Flags when the interval is more than **10%** out of sync.
- `Workload desynchronization`: estimates missing cycles from the I/O-APIC-adjusted APERF delta and the interval desync ratio (CPPC path), or from both desync ratios on the legacy path. Flags when more than **500** cycles appear to be missing.

Desync percentages are formatted without floating-point print support: values are scaled to hundredths and rendered as `%i.%i` (whole and fractional parts) via `format_desync_percent()`.

## Probe flow

### CPPC path (`Run`)

1. `DriverEntry()` builds a CPPC request from `MSR_CPPC_CAPABILITY_1` with `DesPerf` set to nominal (P1) to disable boosting during the measurement window. CPPC is enabled via `MSR_CPPC_ENABLE` if needed.
2. `SANITY_DATA::Run()` saves the current CPPC request, applies the target CPPC settings, and calls `Probe()`.
3. `Probe()` measures read overhead, pauses until `MSR_L3_PACKAGE_ENERGY_STATUS` changes (avoiding a mid-update skew), snapshots all timing sources, then repeatedly reads EFER until the energy MSR changes again. This gives a roughly 10-15 ms window and records how many EFER reads completed in `pm_counter`.
4. `Run()` restores the original CPPC request, computes I/O APIC-normalized deltas and desync ratios across all timing sources (including read-only APERF/MPERF), normalizing both ratios by `pm_counter`.
5. `Run()` then forces P0 (CPPC max perf + `PstateCmd = 0`), reads `EFER.SVME` repeatedly, and evaluates the current P-state against the commanded value.
6. `log_results()` prints SVME state, PM counter, EFER average, and the four desync checks.

### Legacy path (`Run_Legacy`)

Used when CPPC is not supported on the processor:

1. `Probe_Legacy()` gates the EFER read loop on the I/O APIC timer (~25M ticks) instead of the energy MSR.
2. Desync ratios are computed without per-read normalization (`/ pm_counter`).
3. Workload desync includes both interval and TSC desync ratios.
4. Power state elevation is not evaluated.

## Timing sources

`PERFORMANCE_DATA` captures:

- `APERF` / `MPERF`: writable performance counter MSRs.
- `APERF_READ_ONLY` / `MPERF_READ_ONLY`: read-only counterparts used to detect counter shadowing.
- `MSR_TSC`: TSC value read through the TSC MSR.
- `RDTSC`: direct timestamp counter instruction.
- `RDTSCP`: serialized timestamp counter instruction.
- `io_apicTimer`: external timer source read through I/O ports (`io_apic_rtc()`).
- `pstate`: `CurPstate` from `MSR_PSTATE_STATUS` at probe start/end.

## Build

Open `llvm-kmd.sln` in Visual Studio, select **Release | x64** (or Debug), and build. The output is a `.sys` kernel driver built with the **ClangCL** toolset. Load it with your preferred manual mapper or test harness; output appears in the kernel debug log via `DbgPrintEx`.

## Example output

Clean run:

```
Starting sanity check...
 
================================================================================
                                EFER RESULTS
================================================================================
  SVME state                     OFF      
  PM Counter                     2891       2891 expected
  EFER read average              OK         105 cycles           (limit: 800)
  Power state elevation          OK         0 violations         (limit: 1)
  TSC desynchronization          OK         0.0% desync          (limit: 2.5%)
  Interval desynchronization     OK         0.0% desync          (limit: 10%)
  Workload desynchronization     OK         3 cycles             (limit: 500)
--------------------------------------------------------------------------------
  Result: CLEAN  (0/5 checks flagged)
================================================================================
 
Sanity check completed.
```

Flagged run (VM with TSC/APERF counter spoofing):

```
Starting sanity check...
 
================================================================================
                                EFER RESULTS
================================================================================
  SVME state                     OFF      
  PM Counter                     1474       2948 expected
  EFER read average              OK         146 cycles           (limit: 800)
  Power state elevation          FLAGGED    1 violations         (limit: 1)
  TSC desynchronization          FLAGGED    290.90% desync       (limit: 2.5%)
  Interval desynchronization     FLAGGED    289.61% desync       (limit: 10%)
  Workload desynchronization     FLAGGED    13971898 cycles      (limit: 500)
--------------------------------------------------------------------------------
  Result: FLAGGED (4/5 checks flagged)
================================================================================
 
Sanity check completed.
```

## VMM compensation pattern

A common VMM response to EFER reads is to rewind guest-visible TSC clocks and resynchronize APERF/MPERF on VM-exit, keeping all P0-derived TSC sources aligned (APERF/MPERF read-only missed):

```cpp
case MSR::_MSR_EFER:
{
    if (exitInfo1.MSR.isWrite)
    {
        auto efer = MSR::EFER();
        auto tsc = __rdtsc();
        MSR::EFER(efer);
        storage->tsc_step = (__rdtsc() - tsc);
        storage->efer.data.AsUINT64 = gCtx->Rdx << 32 | (ssa->Rax & 0xFFFFFFFF);
    }
    else
    {
        auto tsc = __rdtsc();
        MSR::EFER();
        storage->tsc_step = (__rdtsc() - tsc);
        ssa->Rax = storage->efer.data.AsUINT64 & 0xFFFFFFFF;
        gCtx->Rdx = (storage->efer.data.AsUINT64 >> 32) & 0xFFFFFFFF;
    }
    storage->aperf_last = storage->aperf_init;
    storage->mperf_last = storage->mperf_init;
} break;
```

This is exactly the kind of compensation the desync checks are designed to surface: TSC sources may appear consistent with each other while drifting away from APERF, the I/O APIC timer, and the read-only performance counter MSRs.
