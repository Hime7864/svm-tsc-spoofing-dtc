#include <intrinsics.hpp>



struct PERFORMANCE_DATA
{
    static UINT64 NAKED io_apic_rtc()
    {
        __asm {
            push rdx
            push r8
            push r9

            mov dx, 0x80C
            mov ax, 0x4
            out dx, ax
            mov dx, 0x80B
            in eax, dx
            mov r8, rax
            and r8, 0xFF
            shl r8, 32

            mov dx, 0x80A
            out dx, ax
            mov dx, 0x809
            in eax, dx
            mov r9, rax
            and r9, 0xFFFF
            shl r9, 16
            or r8, r9

            mov dx, 0x808
            out dx, ax
            mov dx, 0x807
            in eax, dx
            mov r9, rax
            and r9, 0xFF00
            or r8, r9

            mov dx, 0x806
            out dx, ax
            mov dx, 0x805
            in eax, dx
            shr eax, 24
            or r8, rax

            mov rax, r8

            pop r9
            pop r8
            pop rdx
            ret
        }
    }
    UINT64 aperf;
    UINT64 mperf;
    UINT64 aperf_r;
    UINT64 mperf_r;
    UINT64 msr_tsc;
    UINT64 io_apicTimer;
    UINT64 rdtsc;
    UINT64 rdtscp;
    UINT64 pstate;

    void read_tsc()
    {
        aperf = MSR::APERF();
        mperf = MSR::MPERF();
		aperf_r = MSR::APERF_READ_ONLY();
		mperf_r = MSR::MPERF_READ_ONLY();
        msr_tsc = MSR::TSC();
        io_apicTimer = io_apic_rtc();
        rdtsc = __rdtsc();
        UINT32 aux = 0;
        rdtscp = _mm_rdtscp(&aux);
        pstate = MSR::PSTATE_STATUS().CurPstate;
        return;
	}

    void diff_tsc(PERFORMANCE_DATA* other)
    {
        aperf -= other->aperf;
        mperf -= other->mperf;
		aperf_r -= other->aperf_r;
		mperf_r -= other->mperf_r;
        msr_tsc -= other->msr_tsc;
        io_apicTimer -= other->io_apicTimer;
        rdtsc -= other->rdtsc;
        rdtscp -= other->rdtscp;
        return;
	}

    void get_overhead(PERFORMANCE_DATA overhead)
    {
        PERFORMANCE_DATA pm0, pm1;
        pm0.read_tsc();
		pm1.read_tsc();
        pm1.diff_tsc(&pm0);
        return;
    }

    void apply_overhead(PERFORMANCE_DATA* output)
    {
		diff_tsc(output);
        return;
	}
};

class SANITY_DATA
{
private:
    PERFORMANCE_DATA pm0;
    PERFORMANCE_DATA pm1;
    UINT64 pm_counter;
    double tsc_desync_ratio;
    double interval_desync_ratio;
    UINT64 rdtsc_delta_ajusted;
    UINT64 reported_cycles;
    UINT64 missing_cycles;
    UINT64 counter_total;
    bool svme_enabled;
    bool pstate_vilolation;

    inline INT64 abs64(INT64 value) { return (value < 0) ? -value : value; }

    inline double fabs(double value) { return (value < 0.0) ? -value : value; }

    void format_desync_percent(double value, char* buffer)
    {
        INT64 scaled = (INT64)(value * 100.0);
        sprintf(buffer, "%i.%i%% desync", (int)(scaled / 100), (int)(scaled % 100));
    }

    void Probe()
    {
        pm0.pstate = MSR::PSTATE_STATUS().CurPstate;

        PERFORMANCE_DATA overhead;
        pm0.get_overhead(overhead);

		//pause since core could be in the middle of an energy status update which would skew results
        UINT64 unit = _mm_readmsr(MSR::_MSR_L3_PACKAGE_ENERGY_STATUS);
        while (unit == _mm_readmsr(MSR::_MSR_L3_PACKAGE_ENERGY_STATUS))
            _mm_pause();

        pm0.read_tsc();

        pm_counter = 0;
        unit = _mm_readmsr(MSR::_MSR_L3_PACKAGE_ENERGY_STATUS);
        while (unit == _mm_readmsr(MSR::_MSR_L3_PACKAGE_ENERGY_STATUS))
        {
            // funny thing here is only about 5% of the work load should be this read
            _mm_readmsr(MSR::_MSR_EFER);
            pm_counter++;
        }

        pm1.read_tsc();
        //  should be forced to 1 via cppc request earlier
        pm1.pstate = MSR::PSTATE_STATUS().CurPstate;
        pm1.apply_overhead(&overhead);

        return;
    }

    void Probe_Legacy()
    {
        pm0.pstate = MSR::PSTATE_STATUS().CurPstate;

        PERFORMANCE_DATA overhead;
        pm0.get_overhead(overhead);

        pm0.read_tsc();

        pm_counter = 0;
        UINT64 unit = PERFORMANCE_DATA::io_apic_rtc() + 25000000;
        while (unit > PERFORMANCE_DATA::io_apic_rtc())
        {
            // funny thing here is only about 5% of the work load should be this read
            _mm_readmsr(MSR::_MSR_EFER);
            pm_counter++;
        }

        pm1.read_tsc();
        //  should be forced to 1 via cppc request earlier
        pm1.pstate = MSR::PSTATE_STATUS().CurPstate;
        pm1.apply_overhead(&overhead);

        return;
    }

public:

    static INT64 get_efer_average()
    {
        auto irql = _mm_readcr8();
        _mm_writecr8(15);
        INT64 total = 0ULL;
        for (int i = 0; i < 1000; i++)
        {
            auto tsc = __rdtsc();
            auto efer = MSR::EFER();
            total += __rdtsc() - tsc;
        }
        _mm_writecr8(irql);
        return total / 1000;
    }

    static bool report_efer_average(UINT64 threshold)
    {
		return (UINT64)get_efer_average() > threshold;
    }

    bool report_power_elevation()
    {
        return pstate_vilolation;
    }

    double get_tsc_desync()
    {
        return fabs(tsc_desync_ratio) * 100.0;
    }

    bool report_tsc_desync(double threshold)
    {
        return get_tsc_desync() > threshold;
    }

    double get_interval_desync()
    {
        return fabs(interval_desync_ratio) * 100.0;
	}

    bool report_interval_desync(double threshold)
    {
        return get_interval_desync() > threshold;
    }

    INT64 get_workload_desync()
    {
        auto batch_reported_cycles = reported_cycles / counter_total;
        auto batch_expected_cycles = (reported_cycles + missing_cycles) / counter_total;
        return abs64(batch_reported_cycles - batch_expected_cycles);
	}

    bool report_workload_desync(INT64 threshold)
    {
        return get_workload_desync() > threshold;
    }

    void Run(MSR_CPPC_REQUEST target_cppc)
    {
        auto old_cppc = MSR::CPPC_REQUEST();
		MSR::CPPC_REQUEST(target_cppc);

        Probe();

		MSR::CPPC_REQUEST(old_cppc);

		auto mperf_delta = pm1.mperf - pm0.mperf;
		auto aperf_delta = pm1.aperf - pm0.aperf;
		auto mperf_r_delta = pm1.mperf_r - pm0.mperf_r;
		auto aperf_r_delta = pm1.aperf_r - pm0.aperf_r;
		auto msr_tsc_delta = pm1.msr_tsc - pm0.msr_tsc;
		auto io_apic_delta = pm1.io_apicTimer - pm0.io_apicTimer;
		auto rdtsc_delta = pm1.rdtsc - pm0.rdtsc;
		auto rdtscp_delta = pm1.rdtscp - pm0.rdtscp;

        auto io_ratio = 920000.0 / (double)io_apic_delta;
        auto expected_p0 = MSR::PSTATE(0).get_frequency_mhz() * 1000;

        auto mperf_delta_ajusted = (UINT64)((double)mperf_delta * io_ratio);
        auto aperf_delta_ajusted = (UINT64)((double)aperf_delta * io_ratio);
        auto mperf_r_delta_ajusted = (UINT64)((double)mperf_r_delta * io_ratio);
        auto aperf_r_delta_ajusted = (UINT64)((double)aperf_r_delta * io_ratio);
        auto msr_tsc_delta_ajusted = (UINT64)((double)msr_tsc_delta * io_ratio);
        auto io_apic_delta_ajusted = (UINT64)((double)io_apic_delta * io_ratio);
        auto rdtsc_delta_ajusted = (UINT64)((double)rdtsc_delta * io_ratio);
        auto rdtscp_delta_ajusted = (UINT64)((double)rdtscp_delta * io_ratio);

        double sync_ratio = 0.0;
        sync_ratio += (double)aperf_delta / (double)aperf_r_delta;
        sync_ratio += (double)mperf_delta / (double)mperf_r_delta;
		sync_ratio += (double)mperf_delta / (double)msr_tsc_delta;
        sync_ratio += (double)mperf_delta / (double)rdtsc_delta;
        sync_ratio += (double)mperf_delta / (double)rdtscp_delta;
        tsc_desync_ratio = (sync_ratio / 5.0) - 1.0;

        double expected_sync_ratio = 0.0;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_r_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)msr_tsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtscp_delta_ajusted;
        interval_desync_ratio = (expected_sync_ratio / 5.0) - 1.0;

        reported_cycles = aperf_delta_ajusted;
        missing_cycles = abs64((UINT64)((double)aperf_delta_ajusted * interval_desync_ratio * tsc_desync_ratio));
		counter_total = (UINT64)((double)pm_counter * io_ratio);

        old_cppc = MSR::CPPC_REQUEST();
        target_cppc.DesPerf = target_cppc.MaxPerf;
        MSR::CPPC_REQUEST(target_cppc);
        MSR_PSTATE_CONTROL cmd;
        cmd.PstateCmd = 0;
		MSR::PSTATE_CONTROL(cmd);
        for (int i = 0; i < pm_counter / 2; i++)
            svme_enabled = MSR::EFER().svme;
        pstate_vilolation = MSR::PSTATE_STATUS().CurPstate == cmd.PstateCmd;
        MSR::CPPC_REQUEST(old_cppc);

        return;
    }

    void Run_Legacy()
    {
        Probe_Legacy();

        auto mperf_delta = pm1.mperf - pm0.mperf;
        auto aperf_delta = pm1.aperf - pm0.aperf;
        auto mperf_r_delta = pm1.mperf_r - pm0.mperf_r;
        auto aperf_r_delta = pm1.aperf_r - pm0.aperf_r;
        auto msr_tsc_delta = pm1.msr_tsc - pm0.msr_tsc;
        auto io_apic_delta = pm1.io_apicTimer - pm0.io_apicTimer;
        auto rdtsc_delta = pm1.rdtsc - pm0.rdtsc;
        auto rdtscp_delta = pm1.rdtscp - pm0.rdtscp;

        auto io_ratio = 920000.0 / (double)io_apic_delta;
        auto expected_p0 = MSR::PSTATE(0).get_frequency_mhz() * 1000;

        auto mperf_delta_ajusted = (UINT64)((double)mperf_delta * io_ratio);
        auto aperf_delta_ajusted = (UINT64)((double)aperf_delta * io_ratio);
        auto mperf_r_delta_ajusted = (UINT64)((double)mperf_r_delta * io_ratio);
        auto aperf_r_delta_ajusted = (UINT64)((double)aperf_r_delta * io_ratio);
        auto msr_tsc_delta_ajusted = (UINT64)((double)msr_tsc_delta * io_ratio);
        auto io_apic_delta_ajusted = (UINT64)((double)io_apic_delta * io_ratio);
        auto rdtsc_delta_ajusted = (UINT64)((double)rdtsc_delta * io_ratio);
        auto rdtscp_delta_ajusted = (UINT64)((double)rdtscp_delta * io_ratio);

        double sync_ratio = 0.0;
        sync_ratio += (double)aperf_delta / (double)aperf_r_delta;
        sync_ratio += (double)mperf_delta / (double)mperf_r_delta;
        sync_ratio += (double)mperf_delta / (double)msr_tsc_delta;
        sync_ratio += (double)mperf_delta / (double)rdtsc_delta;
        sync_ratio += (double)mperf_delta / (double)rdtscp_delta;
        tsc_desync_ratio = (sync_ratio / 5.0) - 1.0;

        double expected_sync_ratio = 0.0;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_r_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)msr_tsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtscp_delta_ajusted;
        interval_desync_ratio = (expected_sync_ratio / 5.0) - 1.0;

        reported_cycles = aperf_delta_ajusted;
        missing_cycles = abs64((UINT64)((double)aperf_delta_ajusted * interval_desync_ratio * tsc_desync_ratio));
        counter_total = (UINT64)((double)pm_counter * io_ratio);

        svme_enabled = MSR::EFER().svme;
        pstate_vilolation = 0;

        return;
    }

    void log_results()
    {
        char detail[128]{};
        int flagged_count = 0;

        printf("\n");
        printf("========================================\n");
        printf("            EFER RESULTS\n");
        printf("========================================\n");

        printf("  %-30s %-9s\n", "SVME state", svme_enabled ? "ON" : "OFF");
        auto io_ratio = 920000.0 / (double)(pm1.io_apicTimer - pm0.io_apicTimer);

        printf("  %-30s %-9i  %llu expected\n", "PM Counter", pm_counter, (UINT64)((double)pm_counter * (1.0 + fabs(interval_desync_ratio) * fabs(tsc_desync_ratio))) / (UINT64)((double)(pm1.aperf - pm0.aperf) * io_ratio));

		auto efer_flagged = report_efer_average(1000);
		sprintf(detail, "%llu %s", get_efer_average(), "cycles");
        printf("  %-30s %-9s  %s (limit: %s)\n",
            "EFER read average",
            efer_flagged ? "FLAGGED" : "OK",
            detail,
			"1000 cycles");
        flagged_count += efer_flagged ? 1 : 0;

        auto elevation_flagged = report_power_elevation();
        sprintf(detail, "%i violations", elevation_flagged ? 1 : 0);
        printf("  %-30s %-9s  %s (limit: %s)\n",
            "Power state elevation",
            elevation_flagged ? "FLAGGED" : "OK",
            detail,
            "1");
        flagged_count += elevation_flagged ? 1 : 0;

        auto tsc_flagged = report_tsc_desync(5.0);
        format_desync_percent(get_tsc_desync(), detail);
        printf("  %-30s %-9s  %s (limit: %s)\n",
            "TSC desynchronization",
            tsc_flagged ? "FLAGGED" : "OK",
            detail,
            "5%");
        flagged_count += tsc_flagged ? 1 : 0;

        auto interval_flagged = report_interval_desync(5.0);
        format_desync_percent(get_interval_desync(), detail);
        printf("  %-30s %-9s  %s (limit: %s)\n",
            "Interval desynchronization",
            interval_flagged ? "FLAGGED" : "OK",
            detail,
            "5%");
        flagged_count += interval_flagged ? 1 : 0;

        auto workload_flagged = report_workload_desync(20);
        sprintf(detail, "%llu cycles", get_workload_desync());
        printf("  %-30s %-9s  %s (limit: %s)\n",
            "Workload desynchronization",
            workload_flagged ? "FLAGGED" : "OK",
            detail,
            "20 cycles");
        flagged_count += workload_flagged ? 1 : 0;

        printf("----------------------------------------\n");
        if (flagged_count == 0)
            printf("  Result: CLEAN  (0/5 checks flagged)\n");
        else
            printf("  Result: FLAGGED (%i/5 checks flagged)\n", flagged_count);
        printf("========================================\n\n");
        return;
    }
};

bool check_suport()
{
    auto feature_ext = CPUID::feature_ext_id_ebx();
    if (feature_ext.CPPC)
    {
        auto cppc = MSR::CPPC_ENABLE();
        if (!cppc.CPPC_En)
        {
            cppc.CPPC_En = 1;
            MSR::CPPC_ENABLE(cppc);
        }
    }
    else
    {
        printf("CPPC is not supported on this processor.\n");
    }
    return feature_ext.CPPC;
}

bool is_amd_cpu()
{
    int cpu_info[4];
    __cpuid(cpu_info, 0);
    return cpu_info[1] == 'htuA';
}

NTSTATUS DriverEntry()
{   
    if (!is_amd_cpu())
    {
		printf("This sanity check is only supported on AMD CPUs.\n");
        return STATUS_NOT_SUPPORTED;
    }
    if (check_suport())
    {
        printf("Starting sanity check...\n");
        MSR_CPPC_REQUEST cppc_request;
        auto cppc_capabilities = MSR::CPPC_CAPABILITY_1();
        cppc_request.MinPerf = cppc_capabilities.LowestPerf;
        cppc_request.MaxPerf = cppc_capabilities.HighestPerf;
        cppc_request.DesPerf = cppc_capabilities.NominalPerf; // This will Set P1 later on in RUN() and disabling boosting

        auto sanity = (SANITY_DATA*)ExAllocatePool(NonPagedPool, sizeof(SANITY_DATA));

        auto irql = _mm_readcr8();
        _mm_writecr8(15);
        sanity->Run(cppc_request);
        _mm_writecr8(irql);

        sanity->log_results();

        ExFreePool(sanity);
        printf("Sanity check completed.\n");
    }
    else
    {
        printf("Starting sanity check...\n");
        auto sanity = (SANITY_DATA*)ExAllocatePool(NonPagedPool, sizeof(SANITY_DATA));

        auto irql = _mm_readcr8();
        _mm_writecr8(15);
        sanity->Run_Legacy();
        _mm_writecr8(irql);

        sanity->log_results();

        ExFreePool(sanity);
        printf("Sanity check completed.\n");
    }

	
    return STATUS_SUCCESS;
}
