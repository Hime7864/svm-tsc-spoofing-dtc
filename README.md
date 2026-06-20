# SVM TSC Spoofing Detection

This project is a research driver designed to detect VMMs that shadow SVME and spoof performance-monitoring timers in order to hide active SVM virtualization.
The detector combines a basic EFER read-overhead check with timing correlation checks that target the subtle missing time introduced by the compensation logic VMMs apply to guest-visible clocks.
It works by correlating two hard-to-spoof, highly consistent time sources:

- `MSR_L3_PACKAGE_ENERGY_STATUS` — a read-only MSR that updates at a steady ~10-15 ms cadence, used to gate the probe window.
- `I/O APIC timer` — an independent hardware timer driven by its own crystal oscillator.

These two sources serve as a ground truth for elapsed time. Using them, the expected cycle count can be calculated and compared against the P0-derived timings to quantify any desyncs introduced by time negation in the guest.

## Architecture

The detection logic lives in two structures in `main.cpp`:

- `PERFORMANCE_DATA` — snapshots and diffs all timing sources (`read_tsc()`, `diff_tsc()`, overhead measurement).
- `SANITY_DATA` — runs the probe, computes desync ratios, and prints results via `log_results()`.

`DriverEntry()` allocates a `SANITY_DATA` instance, raises IRQL to `DISPATCH_LEVEL`, runs the probe, logs results, and frees the buffer.

## Reported checks

The detector currently reports five flagged checks plus two informational lines:

- `SVME state` (informational): after the probe, reads `EFER.SVME` in a loop (`pm_counter / 2` iterations) and prints `ON` or `OFF`. Not counted toward the flagged total.
- `PM Counter` (informational): prints how many EFER reads completed during the gated probe window. Not counted toward the flagged total.
- `EFER read average`: reads EFER 1000 times at elevated IRQL and measures the average RDTSC delta around each read. Flags when the average exceeds 1000 cycles.
- `Power state elevation`: after the probe, forces P0 via CPPC max performance and `PSTATE_CONTROL` (`PstateCmd = 0`), then flags when `CurPstate` matches the commanded P-state.
- `TSC desynchronization`: compares writable APERF/MPERF against their read-only counterparts, then compares MPERF against MSR TSC, RDTSC, and RDTSCP. Flags when the average ratio drift exceeds 5%.
- `Interval desynchronization`: normalizes probe deltas with the I/O APIC timer and compares the measured interval against the expected P0 frequency across MPERF, MPERF read-only, MSR TSC, RDTSC, and RDTSCP. Flags when the interval is more than 5% out of sync.
- `Workload desynchronization`: estimates missing cycles using both desync ratios (`aperf_delta * interval_desync_ratio * tsc_desync_ratio`) and compares APERF-reported batch cycles against the expected workload. Flags when more than 20 cycles per measured batch appear to be missing.

Desync percentages are formatted without floating-point print support: values are scaled to hundredths and rendered as `%i.%i` (whole and fractional parts) via `format_desync_percent()`.

## Probe flow

1. `DriverEntry()` builds a CPPC request from `MSR_CPPC_CAPABILITY_1` with `DesPerf` set to nominal (P1) to disable boosting during the measurement window.
2. `SANITY_DATA::Run()` saves the current CPPC request, applies the target CPPC settings, and calls `Probe()`.
3. `Probe()` measures read overhead, waits for `MSR_L3_PACKAGE_ENERGY_STATUS` to change, snapshots all timing sources, then repeatedly reads EFER until the energy MSR changes again. This gives a roughly 10-15 ms window and records how many EFER reads completed in `pm_counter`.
4. `Run()` computes I/O APIC-normalized deltas and desync ratios across all timing sources, including read-only APERF/MPERF.
5. `Run()` then forces P0 (CPPC max perf + `PstateCmd = 0`), reads `EFER.SVME` repeatedly, and evaluates the current P-state against the commanded value.
6. `log_results()` prints SVME state, PM counter, EFER average, and the four desync checks.

## Timing sources

`PERFORMANCE_DATA` captures:

- `APERF` / `MPERF`: writable performance counter MSRs.
- `APERF_READ_ONLY` / `MPERF_READ_ONLY`: read-only counterparts used to detect counter shadowing.
- `MSR_TSC`: TSC value read through the TSC MSR.
- `RDTSC`: direct timestamp counter instruction.
- `RDTSCP`: serialized timestamp counter instruction.
- `io_apicTimer`: external timer source read through I/O ports.
- `pstate`: current P-state from `MSR_PSTATE_STATUS`.
- `pm_counter`: number of EFER reads completed during the probe interval.

# Example output

Clean run:

```
Starting sanity check...

========================================
            EFER RESULTS
========================================
  SVME state                     OFF
  PM Counter                     8421 cycles
  EFER read average              OK         105 cycles (limit: 1000 cycles)
  Power state elevation          OK         0 violations (limit: 1)
  TSC desynchronization          OK         0.1% desync (limit: 5%)
  Interval desynchronization     OK         0.17% desync (limit: 5%)
  Workload desynchronization     OK         2 cycles (limit: 20 cycles)
----------------------------------------
  Result: CLEAN  (0/5 checks flagged)
========================================

Sanity check completed.
```

Flagged run (VM with TSC/APERF counter spoofing):

```
Starting sanity check...
 
========================================
            EFER RESULTS
========================================
  SVME state                     OFF      
  PM Counter                     1491       21122707744 expected
  EFER read average              OK         168 cycles (limit: 1000 cycles)
  Power state elevation          FLAGGED    1 violations (limit: 1)
  TSC desynchronization          FLAGGED    376352.73% desync (limit: 5%)
  Interval desynchronization     FLAGGED    376423.59% desync (limit: 5%)
  Workload desynchronization     FLAGGED    45303369659 cycles (limit: 20 cycles)
----------------------------------------
  Result: FLAGGED (4/5 checks flagged)
========================================
 
Sanity check completed.
```

# VMM compensation pattern

A common VMM response to EFER reads is to rewind guest-visible TSC clocks and resynchronize APERF/MPERF on VM-exit, keeping all P0-derived TSC sources aligned(APERF/MPERF Read-Only missed):

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
