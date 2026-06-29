#include <intrinsics.hpp>

struct PEFF_CTL
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 EventSelect_low : 8;
            UINT64 UnitMask : 8;
            UINT64 OsUserMode : 2;
            UINT64 Edge : 1;
            UINT64 Reserve0 : 1;
            UINT64 Int : 1;
            UINT64 Reserve1 : 1;
            UINT64 En : 1;
            UINT64 Inv : 1;
            UINT64 CntMask : 8;
            UINT64 EventSelect_high : 4;
            UINT64 Reserve2 : 4;
            UINT64 HostGuestOnly : 2;
            UINT64 Reserve3 : 22;
        };
    };
};

struct PERF_CTR
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 CTR : 48;
            UINT64 Reserved : 16;
        };
    };
};

struct PerfCntrGlobalCtl
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 PerfCntrEn0 : 1;
            UINT64 PerfCntrEn1 : 1;
            UINT64 PerfCntrEn2 : 1;
            UINT64 PerfCntrEn3 : 1;
            UINT64 PerfCntrEn4 : 1;
            UINT64 PerfCntrEn5 : 1;
            UINT64 Reserve0 : 58;
        };
    };
};



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
	UINT64 irperf_extra_cycles;
    bool svme_enabled;
    bool pstate_vilolation;
	UINT64 host_pmc_delta;

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
        return missing_cycles;
	}

    bool report_workload_desync(INT64 threshold)
    {
        return get_workload_desync() > threshold;
    }

    INT64 get_irperf_extra_cycles()
    {
        return irperf_extra_cycles;
	}

    bool report_irperf_extra_cycles(INT64 threshold)
    {
        return get_irperf_extra_cycles() > threshold;
	}

    INT64 get_host_pmc_delta()
    {
        return host_pmc_delta;
	}

    bool report_host_pmc_delta(INT64 threshold)
    {
        return get_host_pmc_delta() > threshold;
    }

    void Run(MSR_CPPC_REQUEST target_cppc)
    {
        PEFF_CTL ctl{ 0 };
        ctl.HostGuestOnly = 2;
        ctl.EventSelect_low = 0xD1;
        ctl.OsUserMode = 3;
        ctl.En = 1;
        _mm_writemsr(0xC0010200, ctl.AsUINT64);

        PERF_CTR ctr_init{ 0 };
        ctr_init.AsUINT64 = _mm_readmsr(0xC0010201);

        auto old_cppc = MSR::CPPC_REQUEST();
		MSR::CPPC_REQUEST(target_cppc);

        auto hwcr = MSR::HWCR();
        hwcr.IRPerfEn = 1;
        MSR::HWCR(hwcr);

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

        auto io_ratio = (920000.0 / (double)io_apic_delta);
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
        tsc_desync_ratio = ((sync_ratio / 5.0) - 1.0) / (double)pm_counter;

        double expected_sync_ratio = 0.0;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)mperf_r_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)msr_tsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtsc_delta_ajusted;
        expected_sync_ratio += (double)expected_p0 / (double)rdtscp_delta_ajusted;
        interval_desync_ratio = ((expected_sync_ratio / 5.0) - 1.0) / (double)pm_counter;

        reported_cycles = aperf_delta_ajusted;
        missing_cycles = abs64((UINT64)((double)aperf_delta_ajusted * interval_desync_ratio));
		counter_total = (UINT64)((double)pm_counter * io_ratio);

        old_cppc = MSR::CPPC_REQUEST();
        target_cppc.DesPerf = target_cppc.MaxPerf;
        MSR::CPPC_REQUEST(target_cppc);
        MSR_PSTATE_CONTROL cmd;
        cmd.PstateCmd = 0;
		MSR::PSTATE_CONTROL(cmd);

        auto irperf = MSR::IRPerfCount();
        for (int i = 0; i < pm_counter / 2; i++)
            _mm_readmsr(MSR::_MSR_EFER);
        auto delta = (MSR::IRPerfCount() - irperf) - 12;
        auto expected_delta = (pm_counter / 2) * 11;
        svme_enabled = MSR::EFER().svme;
		irperf_extra_cycles = (delta - expected_delta) / (pm_counter / 2);
        pstate_vilolation = MSR::PSTATE_STATUS().CurPstate == cmd.PstateCmd;

        MSR::CPPC_REQUEST(old_cppc);

        PERF_CTR ctr{ 0 };
        ctr.AsUINT64 = _mm_readmsr(0xC0010201);

        host_pmc_delta = ctr.CTR - ctr_init.CTR;

        ctl.En = 0;
        _mm_writemsr(0xC0010200, ctl.AsUINT64);
        ctl.AsUINT64 = 0;
        _mm_writemsr(0xC0010200, ctl.AsUINT64);
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
        printf("================================================================================\n");
        printf("                                EFER RESULTS\n");
        printf("================================================================================\n");

        printf("  %-30s %-9s\n", "SVME state", svme_enabled ? "ON" : "OFF");
        auto io_ratio = 920000.0 / (double)(pm1.io_apicTimer - pm0.io_apicTimer);

        printf("  %-30s %-9i  %llu expected\n", "PM Counter", pm_counter, pm_counter * (UINT64)(fabs(tsc_desync_ratio / 2.0) + 1.0));

		auto efer_flagged = report_efer_average(800);
		sprintf(detail, "%llu %s", get_efer_average(), "cycles");
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "EFER read average",
            efer_flagged ? "FLAGGED" : "OK",
            detail,
			"800");
        flagged_count += efer_flagged ? 1 : 0;

        auto elevation_flagged = report_power_elevation();
        sprintf(detail, "%i violations", elevation_flagged ? 1 : 0);
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "Power state elevation",
            elevation_flagged ? "FLAGGED" : "OK",
            detail,
            "1");
        flagged_count += elevation_flagged ? 1 : 0;

        auto tsc_flagged = report_tsc_desync(2.5);
        format_desync_percent(get_tsc_desync(), detail);
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "P0 TSC desync",
            tsc_flagged ? "FLAGGED" : "OK",
            detail,
            "2.5%");
        flagged_count += tsc_flagged ? 1 : 0;

        auto interval_flagged = report_interval_desync(10.0);
        format_desync_percent(get_interval_desync(), detail);
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "Crystal <-> P0 TSC desync",
            interval_flagged ? "FLAGGED" : "OK",
            detail,
            "10%");
        flagged_count += interval_flagged ? 1 : 0;

        auto workload_flagged = report_workload_desync(500);
        sprintf(detail, "%llu cycles", get_workload_desync());
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "APERF cycles missing",
            workload_flagged ? "FLAGGED" : "OK",
            detail,
            "500");
        flagged_count += workload_flagged ? 1 : 0;

		auto irperf_flagged = report_irperf_extra_cycles(1);
		sprintf(detail, "%llu cycles", get_irperf_extra_cycles());
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "IRPerf cycles extra",
            irperf_flagged ? "FLAGGED" : "OK",
            detail,
			"1");
        flagged_count += irperf_flagged ? 1 : 0;

		auto host_pmc_flagged = report_host_pmc_delta(1);
		sprintf(detail, "%llu retired", get_host_pmc_delta());
        printf("  %-30s %-9s  %-20s (limit: %s)\n",
            "PMC cycles extra",
            host_pmc_flagged ? "FLAGGED" : "OK",
			detail,
			"1");
		flagged_count += host_pmc_flagged ? 1 : 0;


        printf("--------------------------------------------------------------------------------\n");
        if (flagged_count == 0)
            printf("  Result: CLEAN  (0/7 checks flagged)\n");
        else
            printf("  Result: FLAGGED (%i/7 checks flagged)\n", flagged_count);
        printf("================================================================================\n\n");
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
        //PEFF_CTL0 test;
        //test.AsUINT64 = _mm_readmsr(0xc0010000);
        //printf("EventSelect0 %i\n", test.EventSelect0);
        //printf("EventSelect1 %i\n", test.EventSelect1);
        //printf("EventSelect2 %i\n", test.EventSelect2);
        //printf("EventSelect3 %i\n", test.EventSelect3);
        //printf("EventSelect4 %i\n", test.EventSelect4);
        //printf("EventSelect5 %i\n", test.EventSelect5);
        //printf("EventSelect6 %i\n", test.EventSelect6);
        //printf("EventSelect7 %i\n", test.EventSelect7);
        //printf("UnitMask %i\n", test.UnitMask);
        //printf("OsUserMode %i\n", test.OsUserMode);
        //printf("Edge %i\n", test.Edge);
        //printf("Int %i\n", test.Int);
        //printf("En %i\n", test.En);
        //printf("Inv %i\n", test.Inv);
        //printf("CntMask %i\n", test.CntMask);
        //printf("EventSelect8 %i\n", test.EventSelect8);
        //printf("EventSelect9 %i\n", test.EventSelect9);
        //printf("EventSelect10 %i\n", test.EventSelect10);
        //printf("EventSelect11 %i\n", test.EventSelect11);
        //printf("HostGuestOnly %i\n", test.HostGuestOnly);
        //
        //test.HostGuestOnly = 1;
        //
        //_mm_writemsr(0xc0010000, test.AsUINT64);

        //printf("C001_0240 %p\n", _mm_readmsr(0xC0010240));
        //printf("C001_0241 %p\n", _mm_readmsr(0xC0010241));
        //
        //PerfCntrGlobalCtl global_ctl;
        //global_ctl.AsUINT64 = _mm_readmsr(0xC0000301);
        //printf("PerfCntrEn0 %i\n", global_ctl.PerfCntrEn0);
        //printf("PerfCntrEn1 %i\n", global_ctl.PerfCntrEn1);
        //printf("PerfCntrEn2 %i\n", global_ctl.PerfCntrEn2);
        //printf("PerfCntrEn3 %i\n", global_ctl.PerfCntrEn3);
        //printf("PerfCntrEn4 %i\n", global_ctl.PerfCntrEn4);
        //printf("PerfCntrEn5 %i\n", global_ctl.PerfCntrEn5);
        //
        //UINT32 msr[10] = {
        //    0xc0010000,0xc0010001,0xc0010002,0xc0010003,
        //    0xc0010200,0xc0010202,0xc0010204,0xc0010206,
        //    0xc0010208,0xc001020A
        //};
        //for (int i = 0; i < 10; i++)
        //{
        //    PEFF_CTL0 test;
        //    test.AsUINT64 = _mm_readmsr(msr[i]);
        //    printf("MSR %x: %llx\n", msr[i], test.AsUINT64);
        //    //printf("EventSelect0 %i\n", test.EventSelect0);
        //    //printf("EventSelect1 %i\n", test.EventSelect1);
        //    //printf("EventSelect2 %i\n", test.EventSelect2);
        //    //printf("EventSelect3 %i\n", test.EventSelect3);
        //    //printf("EventSelect4 %i\n", test.EventSelect4);
        //    //printf("EventSelect5 %i\n", test.EventSelect5);
        //    //printf("EventSelect6 %i\n", test.EventSelect6);
        //    //printf("EventSelect7 %i\n", test.EventSelect7);
        //    //printf("UnitMask %i\n", test.UnitMask);
        //    //printf("OsUserMode %i\n", test.OsUserMode);
        //    //printf("Edge %i\n", test.Edge);
        //    //printf("Int %i\n", test.Int);
        //    //printf("En %i\n", test.En);
        //    //printf("Inv %i\n", test.Inv);
        //    //printf("CntMask %i\n", test.CntMask);
        //    //printf("EventSelect8 %i\n", test.EventSelect8);
        //    //printf("EventSelect9 %i\n", test.EventSelect9);
        //    //printf("EventSelect10 %i\n", test.EventSelect10);
        //    //printf("EventSelect11 %i\n", test.EventSelect11);
        //    //printf("HostGuestOnly %i\n", test.HostGuestOnly);
        //}

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
