#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "platform/platform.h"
#include "debug.h"

// mach_host_self() adds a uref to the host port name on every call; calling it
// per-read leaks refs and can saturate the name's uref count under guests that
// poll /proc/meminfo or /proc/stat at high frequency (e.g. node/V8), after
// which host_info()/host_statistics64() start failing. Resolve it once.
static host_t cached_host_self(void) {
    static host_t host = HOST_NULL;
    if (host == HOST_NULL)
        host = mach_host_self();
    return host;
}

struct cpu_usage get_cpu_usage(void) {
    host_cpu_load_info_data_t load;
    mach_msg_type_number_t fuck = HOST_CPU_LOAD_INFO_COUNT;
    struct cpu_usage usage = {};
    if (host_statistics(cached_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &load, &fuck) != KERN_SUCCESS)
        return usage;
    usage.user_ticks = load.cpu_ticks[CPU_STATE_USER];
    usage.system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM];
    usage.idle_ticks = load.cpu_ticks[CPU_STATE_IDLE];
    usage.nice_ticks = load.cpu_ticks[CPU_STATE_NICE];
    return usage;
}

struct mem_usage get_mem_usage(void) {
    // Last successful reading; served as a fallback so a transient (or
    // OS-beta-induced) host_info failure degrades /proc/meminfo instead of
    // aborting the whole app. Benign data race: stale/torn stats are fine here.
    static struct mem_usage last_good;
    static bool have_last_good = false;

    host_basic_info_data_t basic = {};
    mach_msg_type_number_t fuck = HOST_BASIC_INFO_COUNT;
    kern_return_t status = host_info(cached_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &fuck);
    vm_statistics64_data_t vm = {};
    if (status == KERN_SUCCESS) {
        fuck = HOST_VM_INFO64_COUNT;
        status = host_statistics64(cached_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &fuck);
    }
    if (status != KERN_SUCCESS) {
        printk("WARNING: get_mem_usage: host_info/host_statistics64 failed (kr=%d), using fallback\n", status);
        if (have_last_good)
            return last_good;
        struct mem_usage fallback = {};
        uint64_t memsize = 0;
        size_t size = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &size, NULL, 0) != 0 || memsize == 0)
            memsize = 4ULL * 1024 * 1024 * 1024;
        fallback.total = memsize;
        fallback.free = memsize / 4;
        fallback.active = memsize / 4;
        fallback.inactive = memsize / 4;
        return fallback;
    }

    struct mem_usage usage;
    usage.total = basic.max_mem;
    usage.free = vm.free_count * vm_page_size;
    usage.active = vm.active_count * vm_page_size;
    usage.inactive = vm.inactive_count * vm_page_size;
    last_good = usage;
    have_last_good = true;
    return usage;
}

struct uptime_info get_uptime(void) {
    // A `struct timeval`, and read as one: the seconds and the microseconds are
    // not the same width, so two `uint64_t` put the padding where tv_usec is.
    struct timeval boottime;
    size_t size = sizeof(boottime);
    sysctlbyname("kern.boottime", &boottime, &size, NULL, 0);
    struct timeval now;
    gettimeofday(&now, NULL);

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg;
    size = sizeof(vm_loadavg);
    sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0);

    // linux wants the scale to be 16 bits
    for (int i = 0; i < 3; i++) {
        if (FSHIFT < 16)
            vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
        else
            vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }

    // Hundredths, which is what `uptime_ticks` says and what every reader of it
    // does with it. Whole seconds here made /proc/uptime advance by 0.01 per
    // second, so anything in the guest timing itself against it read zero.
    int64_t sec = (int64_t) now.tv_sec - (int64_t) boottime.tv_sec;
    int64_t usec = (int64_t) now.tv_usec - (int64_t) boottime.tv_usec;

    struct uptime_info uptime = {
        .uptime_ticks = (uint64_t) (sec * 100 + usec / 10000),
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}
