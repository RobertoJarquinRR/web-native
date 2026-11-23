//! Peak working set del proceso actual (solo Windows). Sin dependencias.
//!
//! `K32GetProcessMemoryInfo` acepta `PROCESS_MEMORY_COUNTERS_EX`; se usa esa
//! variante porque es la que admite el tamano mayor en Windows 11.

#[cfg(windows)]
pub fn peak_rss_kb() -> u64 {
    #[repr(C)]
    struct ProcessMemoryCounters {
        cb: u32,
        page_fault_count: u32,
        peak_working_set_size: usize,
        working_set_size: usize,
        quota_peak_paged_pool_usage: usize,
        quota_paged_pool_usage: usize,
        quota_peak_non_paged_pool_usage: usize,
        pagefile_usage: usize,
        peak_pagefile_usage: usize,
    }

    #[repr(C)]
    struct ProcessMemoryCountersEx {
        base: ProcessMemoryCounters,
        private_usage: usize,
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentProcess() -> isize;
        fn K32GetProcessMemoryInfo(
            process: isize,
            counters: *mut ProcessMemoryCounters,
            size: u32,
        ) -> i32;
    }

    let mut counters: ProcessMemoryCountersEx = unsafe { std::mem::zeroed() };
    let size = std::mem::size_of::<ProcessMemoryCountersEx>() as u32;
    counters.base.cb = size;
    let ok = unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut counters.base, size) };
    if ok == 0 {
        0
    } else {
        (counters.base.peak_working_set_size / 1024) as u64
    }
}

#[cfg(not(windows))]
pub fn peak_rss_kb() -> u64 {
    0
}
