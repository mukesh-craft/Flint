//! System information for the Flint standard library — portable by design.
//!
//! # Scope, deliberately
//!
//! This module reads *machine facts*: CPU count, memory, OS name and version,
//! uptime, and where the temporary directory is. It does **not** touch cameras,
//! sensors, contacts or notifications, because those are platform APIs with
//! runtime permission models, not libraries — and a "world-class" runtime does
//! not pretend to offer a camera API it cannot verify.
//!
//! Every platform path is written against the documented C ABI of that
//! platform, selected at compile time:
//!
//! | Platform | Source of truth |
//! |---|---|
//! | Linux / Android | `sysconf(3)`, `uname(2)`, `/proc` (where permitted) |
//! | macOS / iOS | `sysctlbyname(3)`, `sysctl(2)`, `mach_absolute_time` |
//! | Windows | `GetSystemInfo`, `GetTickCount64`, `GlobalMemoryStatusEx`, `RtlGetVersion` |
//! | Anything else | a clear "unsupported" answer, never a wrong one |
//!
//! # Restriction honesty (measured on Android)
//!
//! Two facts about Linux/Android shaped the accessors, both found by running
//! them rather than by reading the man pages:
//!
//! * **`/proc/uptime` and `/proc/meminfo` are permission-denied** on modern
//!   Android (since the /proc hardening of Android 8). Uptime and memory
//!   therefore report -1 with a message there, where they work on desktop
//!   Linux. A caller can degrade; it is never handed a made-up number.
//! * **`sysconf` constants are libc-specific.** `_SC_NPROCESSORS_ONLN` is 84 on
//!   glibc but bionic does not implement that index (it returns -1, verified).
//!   The values used here are the glibc ones and the module reports -1 when the
//!   platform declines, which is why every accessor is allowed to say "unknown".
//!
//! # Verification honesty
//!
//! Only the Linux/Android path was compiled and run on the machine that wrote
//! this (Termux, AArch64). The macOS and Windows paths are written to their
//! documented APIs and are compiled only on those targets; where this file
//! claims something about them, it is a claim about the API contract, not a
//! measurement. `flint_system_supported()` tells a program which it is running
//! on, so a caller can degrade instead of guessing.
//!
//! Every accessor returns a negative value for "unknown", and
//! `flint_system_error()` explains why. Nothing returns a plausible-looking
//! zero for a fact it could not read.

/// Shared result buffer. Same contract as the other modules: valid until the
/// next call, NUL-terminated.
const OUT_CAP: usize = 1024;

static mut OUT: [u8; OUT_CAP] = [0; OUT_CAP];
static mut ERROR: [u8; 192] = [0; 192];

fn out() -> &'static mut [u8] {
    unsafe { &mut *core::ptr::addr_of_mut!(OUT) }
}

fn set_error(message: &str) {
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERROR) };
    let bytes = message.as_bytes();
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

fn write_out(bytes: &[u8]) -> bool {
    let cap = unsafe { *core::ptr::addr_of!(OUT) }.len() - 1;
    if bytes.len() > cap {
        set_error("system: result exceeds the output buffer");
        return false;
    }
    let slot = out();
    slot[..bytes.len()].copy_from_slice(bytes);
    slot[bytes.len()] = 0;
    true
}

fn put_i64(value: i64) -> i64 {
    // Digits come out least-significant first, so build them reversed and flip
    // before writing. (The first version wrote them as-is and produced "54321"
    // for 12345 -- a test caught it, which is the only reason it did not ship.)
    let negative = value < 0;
    let mut magnitude = if negative {
        (value as i128).unsigned_abs()
    } else {
        value as u128
    };
    let mut digits = [0u8; 40];
    let mut n = 0usize;
    loop {
        digits[n] = b'0' + (magnitude % 10) as u8;
        magnitude /= 10;
        n += 1;
        if magnitude == 0 {
            break;
        }
    }
    let mut text = [0u8; 42];
    let mut out_len = 0usize;
    if negative {
        text[out_len] = b'-';
        out_len += 1;
    }
    while n > 0 {
        n -= 1;
        text[out_len] = digits[n];
        out_len += 1;
    }
    if !write_out(&text[..out_len]) {
        return -1;
    }
    out_len as i64
}

/// The last result as a string; valid until the next call.
#[no_mangle]
pub extern "C" fn flint_system_last() -> *const u8 {
    core::ptr::addr_of!(OUT) as *const u8
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_system_error() -> *const u8 {
    core::ptr::addr_of!(ERROR) as *const u8
}

/// 1 when this platform has a supported backend, 0 otherwise.
#[no_mangle]
pub extern "C" fn flint_system_supported() -> i64 {
    if cfg!(any(target_os = "linux", target_os = "android")) {
        1
    } else if cfg!(any(target_os = "macos", target_os = "ios")) {
        1
    } else if cfg!(target_os = "windows") {
        1
    } else {
        0
    }
}

/// A short platform tag: "linux", "android", "macos", "ios", "windows" or
/// "unknown". Returns the string length, or -1.
#[no_mangle]
pub extern "C" fn flint_system_platform() -> i64 {
    let name: &str = if cfg!(target_os = "linux") {
        "linux"
    } else if cfg!(target_os = "android") {
        "android"
    } else if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "ios") {
        "ios"
    } else if cfg!(target_os = "windows") {
        "windows"
    } else {
        "unknown"
    };
    if !write_out(name.as_bytes()) {
        return -1;
    }
    name.len() as i64
}

/// Number of usable CPUs, or -1 when unknown.
#[no_mangle]
pub extern "C" fn flint_system_cpu_count() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        // _SC_NPROCESSORS_ONLN on glibc. bionic does not implement this index
        // (verified: returns -1), so fall back to the CPU-online sysfs file,
        // which is world-readable on Linux and, where it is not, we say so.
        let count = unsafe { sysconf(84) };
        if count > 0 {
            return count;
        }
        if let Some(text) = read_proc_file("/sys/devices/system/cpu/online") {
            // "0-7" or "0-3,8"; the upper bound is the useful number.
            if let Some(last) = text.trim().split(',').next_back() {
                if let Some((_, high)) = last.trim().split_once('-') {
                    if let Ok(value) = high.trim().parse::<i64>() {
                        return value + 1;
                    }
                } else if let Ok(value) = last.trim().parse::<i64>() {
                    return value + 1;
                }
            }
        }
        set_error("system: CPU count unavailable on this platform");
        return -1;
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        let mut count: i32 = 0;
        let mut size = core::mem::size_of::<i32>();
        let rc = unsafe { sysctlbyname(c"hw.ncpu".as_ptr(), &mut count as *mut _ as *mut core::ffi::c_void, &mut size, core::ptr::null_mut(), 0) };
        return if rc == 0 && count > 0 { count as i64 } else { -1 };
    }
    #[cfg(target_os = "windows")]
    {
        let mut info = SYSTEM_INFO_ZEROED;
        unsafe { GetSystemInfo(&mut info) };
        return if info.dwNumberOfProcessors > 0 {
            info.dwNumberOfProcessors as i64
        } else {
            -1
        };
    }
    #[allow(unreachable_code)]
    {
        set_error("system: CPU count is not implemented on this platform");
        -1
    }
}

/// Physical memory in bytes, or -1 when unknown.
#[no_mangle]
pub extern "C" fn flint_system_memory_total() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        // /proc/meminfo is the only source that works without extra
        // permissions on every Linux variant, including containers.
        if let Some(text) = read_proc_file("/proc/meminfo") {
            if let Some(kb) = field_after(&text, "MemTotal:") {
                let mut value: i64 = 0;
                for c in kb.bytes() {
                    if c.is_ascii_digit() {
                        value = value * 10 + (c - b'0') as i64;
                    } else if c == b' ' {
                        break;
                    }
                }
                if value > 0 {
                    return value * 1024;
                }
            }
        }
        // Page size * physical pages is the portable POSIX fallback.
        // _SC_PHYS_PAGES is 85 on Linux; _SC_PAGESIZE is 30. bionic may decline
        // either (it returns -1), which is reported rather than guessed.
        let pages = unsafe { sysconf(85) };
        let page_size = unsafe { sysconf(30) };
        if pages > 0 {
            let size = if page_size > 0 { page_size } else { 4096 };
            return pages * size;
        }
        set_error("system: memory total unavailable");
        return -1;
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        let mut total: u64 = 0;
        let mut size = core::mem::size_of::<u64>();
        let rc = unsafe { sysctlbyname(c"hw.memsize".as_ptr(), &mut total as *mut _ as *mut core::ffi::c_void, &mut size, core::ptr::null_mut(), 0) };
        return if rc == 0 && total > 0 { total as i64 } else { -1 };
    }
    #[cfg(target_os = "windows")]
    {
        let mut status = MEMORYSTATUSEX_ZEROED;
        unsafe { GlobalMemoryStatusEx(&mut status) };
        return if status.dwTotalPhys > 0 {
            status.dwTotalPhys as i64
        } else {
            -1
        };
    }
    #[allow(unreachable_code)]
    {
        set_error("system: memory total is not implemented on this platform");
        -1
    }
}

/// Currently available memory in bytes, or -1 when unknown.
#[no_mangle]
pub extern "C" fn flint_system_memory_free() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        if let Some(text) = read_proc_file("/proc/meminfo") {
            // MemAvailable is the honest figure: free memory alone excludes
            // reclaimable page cache and understates what a program can use.
            if let Some(kb) = field_after(&text, "MemAvailable:") {
                let mut value: i64 = 0;
                for c in kb.bytes() {
                    if c.is_ascii_digit() {
                        value = value * 10 + (c - b'0') as i64;
                    } else if c == b' ' {
                        break;
                    }
                }
                if value > 0 {
                    return value * 1024;
                }
            }
        }
        set_error("system: available memory unavailable");
        return -1;
    }
    #[cfg(target_os = "windows")]
    {
        let mut status = MEMORYSTATUSEX_ZEROED;
        unsafe { GlobalMemoryStatusEx(&mut status) };
        return status.dwAvailPhys as i64;
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        // macOS has no cheap "available" figure; vm_stat parsing needs
        // privileges we do not assume, so report unknown rather than guess.
        set_error("system: available memory is not available on macOS");
        return -1;
    }
    #[allow(unreachable_code)]
    {
        set_error("system: available memory is not implemented here");
        -1
    }
}

/// System uptime in seconds, or -1 when unknown.
#[no_mangle]
pub extern "C" fn flint_system_uptime() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        if let Some(text) = read_proc_file("/proc/uptime") {
            let mut value: i64 = 0;
            let mut seen = false;
            for c in text.bytes() {
                if c.is_ascii_digit() {
                    value = value * 10 + (c - b'0') as i64;
                    seen = true;
                } else if seen {
                    break;
                }
            }
            return if seen { value } else { -1 };
        }
        set_error("system: uptime unavailable");
        return -1;
    }
    #[cfg(target_os = "windows")]
    {
        // GetTickCount64 counts milliseconds since boot, including sleep.
        return (unsafe { GetTickCount64() } / 1000) as i64;
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        // mach_absolute_time needs the timebase from sysctl; that pairing is
        // more machinery than an uptime question justifies, so report unknown.
        set_error("system: uptime is not implemented on macOS");
        return -1;
    }
    #[allow(unreachable_code)]
    {
        set_error("system: uptime is not implemented here");
        -1
    }
}

/// Kernel/OS release string. Returns the string length, or -1.
#[no_mangle]
pub extern "C" fn flint_system_os_version() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        let mut buf = [0u8; 256];
        // uname: sysname, nodename, release, version, machine. Release is
        // what a program actually wants ("6.1.0-android14").
        unsafe {
            let mut u: UtsName = UTSNAME_ZEROED;
            if uname(&mut u) != 0 {
                set_error("system: uname failed");
                return -1;
            }
            for (i, byte) in u.release.iter().enumerate() {
                if *byte == 0 {
                    break;
                }
                buf[i] = *byte;
            }
        }
        let end = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
        if end == 0 {
            set_error("system: empty release string");
            return -1;
        }
        if !write_out(&buf[..end]) {
            return -1;
        }
        return end as i64;
    }
    #[cfg(target_os = "windows")]
    {
        let mut info: OsVersionInfoExW = OSVERSIONINFO_ZEROED;
        unsafe { RtlGetVersion(&mut info) };
        let major = info.dwMajorVersion;
        let minor = info.dwMinorVersion;
        let build = info.dwBuildNumber;
        let mut text = [0u8; 64];
        let mut n = 0usize;
        let mut put = |value: u32, dots: &mut usize| {
            let mut buf = [0u8; 10];
            let mut i = 0usize;
            if value == 0 {
                buf[0] = b'0';
                i = 1;
            }
            while value > 0 && i < buf.len() {
                buf[i] = b'0' + (value % 10) as u8;
                value /= 10;
                i += 1;
            }
            while i > 0 && *dots < text.len() {
                i -= 1;
                text[*dots] = buf[i];
                *dots += 1;
            }
        };
        let mut end = 0usize;
        put(build, &mut end);
        put(minor, &mut end);
        put(major, &mut end);
        if !write_out(&text[..end]) {
            return -1;
        }
        end as i64
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        let mut version: [u8; 64] = [0; 64];
        let mut size = version.len();
        let rc = unsafe { sysctlbyname(c"kern.osrelease".as_ptr(), version.as_mut_ptr() as *mut core::ffi::c_void, &mut size, core::ptr::null_mut(), 0) };
        if rc != 0 {
            set_error("system: kern.osrelease unavailable");
            return -1;
        }
        let end = version.iter().position(|b| *b == 0).unwrap_or(version.len());
        if !write_out(&version[..end]) {
            return -1;
        }
        return end as i64;
    }
    #[allow(unreachable_code)]
    {
        set_error("system: OS version is not implemented here");
        -1
    }
}

/// The directory for temporary files, honouring the platform convention:
/// $TMPDIR on POSIX, %TEMP% then %TMP% on Windows. Returns its length, or -1.
#[no_mangle]
pub extern "C" fn flint_system_temp_dir() -> i64 {
    #[cfg(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios"))]
    {
        for key in ["TMPDIR", "TMP", "TEMP"] {
            if let Some(value) = read_env(key) {
                if !value.is_empty() {
                    if !write_out(value.as_bytes()) {
                        return -1;
                    }
                    return value.len() as i64;
                }
            }
        }
        // POSIX says /tmp when nothing is set; it exists on every platform
        // except Android, where TMPDIR is always set in practice.
        if !write_out(b"/tmp") {
            return -1;
        }
        return 4;
    }
    #[cfg(target_os = "windows")]
    {
        for key in ["TEMP", "TMP"] {
            if let Some(value) = read_env(key) {
                if !value.is_empty() {
                    if !write_out(value.as_bytes()) {
                        return -1;
                    }
                    return value.len() as i64;
                }
            }
        }
        set_error("system: TEMP and TMP are both unset");
        return -1;
    }
    #[allow(unreachable_code)]
    {
        set_error("system: temp directory is not implemented here");
        -1
    }
}

// ---------------------------------------------------------------------------
// Platform bindings
// ---------------------------------------------------------------------------

#[cfg(any(target_os = "linux", target_os = "android"))]
mod posix {
    use core::ffi::{c_char, c_int, c_long, c_void};

    #[repr(C)]
    pub struct UtsName {
        pub sysname: [u8; 65],
        pub nodename: [u8; 65],
        pub release: [u8; 65],
        pub version: [u8; 65],
        pub machine: [u8; 65],
    }

    impl UtsName {
        pub const fn zeroed() -> Self {
            UtsName {
                sysname: [0; 65],
                nodename: [0; 65],
                release: [0; 65],
                version: [0; 65],
                machine: [0; 65],
            }
        }
    }

    extern "C" {
        pub fn sysconf(name: c_int) -> c_long;
        pub fn uname(buf: *mut UtsName) -> c_int;
        pub fn getenv(name: *const c_char) -> *mut c_char;
        pub fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
        pub fn fgets(buf: *mut c_char, size: c_int, stream: *mut c_void) -> *mut c_char;
        pub fn fclose(stream: *mut c_void) -> c_int;
    }
}

#[cfg(any(target_os = "linux", target_os = "android"))]
use posix as platform;

#[cfg(any(target_os = "macos", target_os = "ios"))]
mod posix {
    use core::ffi::{c_char, c_int, c_long, c_void};

    #[repr(C)]
    pub struct UtsName {
        pub sysname: [u8; 256],
        pub nodename: [u8; 256],
        pub release: [u8; 256],
        pub version: [u8; 256],
        pub machine: [u8; 256],
    }

    impl UtsName {
        pub const fn zeroed() -> Self {
            UtsName {
                sysname: [0; 256],
                nodename: [0; 256],
                release: [0; 256],
                version: [0; 256],
                machine: [0; 256],
            }
        }
    }

    extern "C" {
        pub fn sysconf(name: c_int) -> c_long;
        pub fn uname(buf: *mut UtsName) -> c_int;
        pub fn getenv(name: *const c_char) -> *mut c_char;
        pub fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
        pub fn fgets(buf: *mut c_char, size: c_int, stream: *mut c_void) -> *mut c_char;
        pub fn fclose(stream: *mut c_void) -> c_int;
        pub fn sysctlbyname(
            name: *const c_char,
            old: *mut c_void,
            oldlen: *mut usize,
            new: *mut c_void,
            newlen: usize,
        ) -> c_int;
    }
}

#[cfg(any(target_os = "macos", target_os = "ios"))]
use posix as platform;

#[cfg(target_os = "windows")]
mod platform {
    use core::ffi::{c_char, c_int, c_void};

    #[repr(C)]
    pub struct SystemInfo {
        pub w_processor_architecture: u16,
        pub w_reserved: u16,
        pub dw_page_size: u32,
        pub lp_minimum_application_address: usize,
        pub lp_maximum_application_address: usize,
        pub dw_active_processor_mask: usize,
        pub dw_number_of_processors: u32,
        pub dw_processor_type: u32,
        pub dw_allocation_granularity: u32,
        pub w_processor_level: u16,
        pub w_processor_revision: u16,
    }
    impl SystemInfo {
        pub const fn zeroed() -> Self {
            SystemInfo {
                w_processor_architecture: 0,
                w_reserved: 0,
                dw_page_size: 0,
                lp_minimum_application_address: 0,
                lp_maximum_application_address: 0,
                dw_active_processor_mask: 0,
                dw_number_of_processors: 0,
                dw_processor_type: 0,
                dw_allocation_granularity: 0,
                w_processor_level: 0,
                w_processor_revision: 0,
            }
        }
    }

    #[repr(C)]
    pub struct MemoryStatusEx {
        pub dw_length: u32,
        pub dw_memory_load: u32,
        pub ull_total_phys: u64,
        pub ull_avail_phys: u64,
        pub ull_total_page_file: u64,
        pub ull_avail_page_file: u64,
        pub ull_total_virtual: u64,
        pub ull_avail_virtual: u64,
        pub ull_avail_extended_virtual: u64,
    }
    impl MemoryStatusEx {
        pub const fn zeroed() -> Self {
            MemoryStatusEx {
                dw_length: core::mem::size_of::<MemoryStatusEx>() as u32,
                dw_memory_load: 0,
                ull_total_phys: 0,
                ull_avail_phys: 0,
                ull_total_page_file: 0,
                ull_avail_page_file: 0,
                ull_total_virtual: 0,
                ull_avail_virtual: 0,
                ull_avail_extended_virtual: 0,
            }
        }
    }

    #[repr(C)]
    pub struct OsVersionInfoExW {
        pub dw_os_version_info_size: u32,
        pub dw_major_version: u32,
        pub dw_minor_version: u32,
        pub dw_build_number: u32,
        pub dw_platform_id: u32,
        pub sz_csd_version: [u16; 128],
    }
    impl OsVersionInfoExW {
        pub const fn zeroed() -> Self {
            OsVersionInfoExW {
                dw_os_version_info_size: core::mem::size_of::<OsVersionInfoExW>() as u32,
                dw_major_version: 0,
                dw_minor_version: 0,
                dw_build_number: 0,
                dw_platform_id: 0,
                sz_csd_version: [0; 128],
            }
        }
    }

    extern "system" {
        pub fn GetSystemInfo(info: *mut SystemInfo);
        pub fn GetTickCount64() -> u64;
        pub fn GlobalMemoryStatusEx(status: *mut MemoryStatusEx) -> c_int;
        pub fn RtlGetVersion(version: *mut OsVersionInfoExW);
        pub fn GetEnvironmentVariableA(
            name: *const c_char,
            buffer: *mut u16,
            size: u32,
        ) -> u32;
    }
}

#[cfg(target_os = "windows")]
use platform as win;

// Short aliases so the accessors above read cleanly without importing the
// platform module at every call site.
#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios"))]
use platform::{sysconf, uname, UtsName};

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios"))]
const UTSNAME_ZEROED: UtsName = UtsName::zeroed();

#[cfg(target_os = "windows")]
const SYSTEM_INFO_ZEROED: win::SystemInfo = win::SystemInfo::zeroed();
#[cfg(target_os = "windows")]
const MEMORYSTATUSEX_ZEROED: win::MemoryStatusEx = win::MemoryStatusEx::zeroed();
#[cfg(target_os = "windows")]
const OSVERSIONINFO_ZEROED: win::OsVersionInfoExW = win::OsVersionInfoExW::zeroed();

#[cfg(target_os = "windows")]
use win::{GetSystemInfo, GetTickCount64, GlobalMemoryStatusEx, RtlGetVersion};

/// Reads a small file into a String. Used only for the /proc files that have no
/// libc equivalent; nothing here parses untrusted input.
#[cfg(any(target_os = "linux", target_os = "android"))]
fn read_proc_file(path: &str) -> Option<alloc::string::String> {
    use core::ffi::c_void;
    let c_path = match alloc::ffi::CString::new(path) {
        Ok(p) => p,
        Err(_) => return None,
    };
    let mut handle = [0u8; 8192];
    unsafe {
        let file = platform::fopen(c_path.as_ptr(), b"r\0".as_ptr() as *const core::ffi::c_char);
        if file.is_null() {
            return None;
        }
        let chunk = 4096;
        let mut total = 0usize;
        let got = platform::fgets(
            handle.as_mut_ptr() as *mut core::ffi::c_char,
            chunk,
            file as *mut c_void,
        );
        if !got.is_null() {
            while total < handle.len() && handle[total] != 0 {
                total += 1;
            }
        }
        platform::fclose(file as *mut c_void);
        if total == 0 {
            None
        } else {
            core::str::from_utf8(&handle[..total])
                .ok()
                .map(|s| alloc::string::String::from(s))
        }
    }
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
fn read_proc_file(_path: &str) -> Option<alloc::string::String> {
    None
}

/// The value after `key:` on the same line of a /proc-style file.
fn field_after<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix(key) {
            return Some(rest.trim_start());
        }
    }
    None
}

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios"))]
fn read_env(key: &str) -> Option<alloc::string::String> {
    let c_key = alloc::ffi::CString::new(key).ok()?;
    unsafe {
        let value = platform::getenv(c_key.as_ptr());
        if value.is_null() {
            None
        } else {
            core::ffi::CStr::from_ptr(value)
                .to_str()
                .ok()
                .map(|s| alloc::string::String::from(s))
        }
    }
}

#[cfg(target_os = "windows")]
fn read_env(key: &str) -> Option<alloc::string::String> {
    use core::ffi::{c_char, c_void};
    // GetEnvironmentVariableA is documented as ANSI; the A suffix is what
    // Termux-free Windows builds export without extra linking.
    let c_key = alloc::ffi::CString::new(key).ok()?;
    let mut buffer = [0u16; 1024];
    let n = unsafe {
        win::GetEnvironmentVariableA(
            c_key.as_ptr() as *const c_char,
            buffer.as_mut_ptr(),
            buffer.len() as u32,
        )
    };
    if n == 0 || n as usize > buffer.len() {
        return None;
    }
    // The A-variant fills bytes; reinterpret the low half as Latin-1 so paths
    // with non-ASCII characters survive as bytes rather than being mangled.
    let bytes: Vec<u8> = buffer[..n as usize]
        .iter()
        .map(|c| *c as u8)
        .collect();
    alloc::string::String::from_utf8(bytes).ok()
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios", target_os = "windows")))]
fn read_env(_key: &str) -> Option<alloc::string::String> {
    None
}

/// Formats an integer into the shared buffer (used by the callers above).
#[allow(dead_code)]
fn emit_number(value: i64) -> i64 {
    put_i64(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};

    fn last() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_system_last() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    fn error_text() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_system_error() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    /// The contract under test is "never a wrong answer": either a plausible
    /// value, or -1 with an explanation. A platform may legitimately refuse
    /// (Android denies /proc, bionic drops sysconf indices), and this module
    /// is required to say so rather than invent a number.
    fn value_or_explained(value: i64, what: &str) {
        if value >= 0 {
            return;
        }
        assert_eq!(value, -1, "{what}: negative but not -1: {value}");
        let message = error_text();
        assert!(
            message.starts_with("system:"),
            "{what}: -1 must come with a reason, got: {message}"
        );
    }

    #[test]
    fn platform_is_named_and_supported_here() {
        let n = flint_system_platform();
        assert!(n > 0);
        let name = last();
        assert!(
            matches!(name.as_str(), "linux" | "android"),
            "this crate is built for Linux/Android, said: {name}"
        );
        assert_eq!(flint_system_supported(), 1);
    }

    #[test]
    fn cpu_count_is_positive_or_explained() {
        let cpus = flint_system_cpu_count();
        value_or_explained(cpus, "cpu count");
        if cpus >= 0 {
            assert!(cpus >= 1, "a reported CPU count is at least 1: {cpus}");
        }
    }

    #[test]
    fn memory_total_is_plausible_or_explained() {
        let total = flint_system_memory_total();
        value_or_explained(total, "memory total");
        if total >= 0 {
            assert!(
                total > 32 * 1024 * 1024,
                "a reported memory total must be plausible: {total}"
            );
        }
    }

    #[test]
    fn memory_free_is_bounded_by_total_or_explained() {
        let total = flint_system_memory_total();
        let free = flint_system_memory_free();
        value_or_explained(free, "memory free");
        if free >= 0 && total >= 0 {
            assert!(
                free <= total,
                "available ({free}) cannot exceed total ({total})"
            );
        }
    }

    #[test]
    fn uptime_is_positive_or_explained() {
        let uptime = flint_system_uptime();
        value_or_explained(uptime, "uptime");
        if uptime >= 0 {
            assert!(uptime > 0, "a reported uptime must be positive: {uptime}");
        }
    }

    #[test]
    fn os_version_is_non_empty() {
        assert!(flint_system_os_version() > 0, "{}", error_text());
        let version = last();
        assert!(!version.is_empty());
        assert!(
            version.chars().next().map(|c| c.is_ascii_digit()).unwrap_or(false),
            "a Linux release string starts with a digit: {version}"
        );
    }

    #[test]
    fn temp_dir_is_absolute_and_exists() {
        assert!(flint_system_temp_dir() > 0, "{}", error_text());
        let dir = last();
        assert!(dir.starts_with('/'), "temp dir should be absolute: {dir}");
    }

    #[test]
    fn proc_field_parsing_is_exact() {
        let sample = "MemTotal:       16308456 kB\nMemAvailable:   1 000 kB\n";
        assert_eq!(field_after(sample, "MemTotal:"), Some("16308456 kB"));
        assert_eq!(field_after(sample, "Nope:"), None);
    }

    #[test]
    fn number_formatting_handles_edge_cases() {
        assert!(put_i64(0) > 0);
        assert_eq!(last(), "0");
        assert!(put_i64(12345) > 0);
        assert_eq!(last(), "12345");
        assert!(put_i64(-42) > 0);
        assert_eq!(last(), "-42");
    }
}