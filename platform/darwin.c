#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "platform/platform.h"

struct cpu_usage get_cpu_usage() {
    host_cpu_load_info_data_t load;
    mach_msg_type_number_t fuck = HOST_CPU_LOAD_INFO_COUNT;
    host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &load, &fuck);
    struct cpu_usage usage;
    usage.user_ticks = load.cpu_ticks[CPU_STATE_USER];
    usage.system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM];
    usage.idle_ticks = load.cpu_ticks[CPU_STATE_IDLE];
    usage.nice_ticks = load.cpu_ticks[CPU_STATE_NICE];
    return usage;
}

struct mem_usage get_mem_usage() {
    host_basic_info_data_t basic = {};
    mach_msg_type_number_t fuck = HOST_BASIC_INFO_COUNT;
    kern_return_t status = host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &fuck);
    assert(status == KERN_SUCCESS);
    vm_statistics64_data_t vm = {};
    fuck = HOST_VM_INFO64_COUNT;
    status = host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &fuck);
    assert(status == KERN_SUCCESS);

    struct mem_usage usage;
    usage.total = basic.max_mem;
    usage.free = vm.free_count * vm_page_size;
    usage.active = vm.active_count * vm_page_size;
    usage.inactive = vm.inactive_count * vm_page_size;
    return usage;
}

struct uptime_info get_uptime() {
    struct timeval now;
    gettimeofday(&now, NULL);

    static struct timeval kern_boottime;
    static bool kern_boottime_initialized;
    if (!kern_boottime_initialized) {
        size_t size = sizeof(kern_boottime);
        if (sysctlbyname("kern.boottime", &kern_boottime, &size, NULL, 0) < 0 ||
                kern_boottime.tv_sec == 0) {
            kern_boottime = now;
        }
        kern_boottime_initialized = true;
    }

    static uint32_t cached_loadavg[3];
    static time_t cached_loadavg_sec = -1;
    if (cached_loadavg_sec != now.tv_sec) {
        struct {
            uint32_t ldavg[3];
            long scale;
        } vm_loadavg = {};
        size_t size = sizeof(vm_loadavg);
        sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0);

        // linux wants the scale to be 16 bits
        for (int i = 0; i < 3; i++) {
            if (FSHIFT < 16)
                vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
            else
                vm_loadavg.ldavg[i] >>= FSHIFT - 16;
            cached_loadavg[i] = vm_loadavg.ldavg[i];
        }
        cached_loadavg_sec = now.tv_sec;
    }

    struct timeval elapsed;
    timersub(&now, &kern_boottime, &elapsed);

    struct uptime_info uptime = {
        .uptime_ticks = (uint64_t) elapsed.tv_sec * 100 + elapsed.tv_usec / 10000,
        .load_1m = cached_loadavg[0],
        .load_5m = cached_loadavg[1],
        .load_15m = cached_loadavg[2],
    };
    return uptime;
}
