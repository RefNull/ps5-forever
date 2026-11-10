/* va-probe-native: measures large virtual-address reservations from a payload
 * process, outside the title. For 1-32 GiB it tries four methods, logs each
 * result and placement, and releases it; the fixed-address method also tries
 * 64 GiB to 1 TiB, holding each success for 10 s. Then it holds the largest
 * success of each method (30 s, or 10 s above 32 GiB) between marker lines,
 * so the console's periodic "VM Stats ... page table CPU" line can be
 * compared. Logs go to the console log, prefixed "VAPROBE-NATIVE ".
 *
 * Fixed reserves are placed from the VM map read just before each one: above
 * every existing mapping and aligned to the size when that fits below
 * USER_TOP, else in the lowest free gap at or above MIN_FIXED_ADDRESS that is
 * size-aligned, else in the lowest such gap at the reserve alignment.
 *
 * sceKernelGetPageTableStats is exported by the SDK's libkernel stubs but no
 * SDK header declares it. It is called with four zeroed 256-byte buffers and
 * the first four 32-bit words of each are logged raw ("ptstats"), without
 * assuming what they mean. Build with -DVAPROBE_NO_PTSTATS to leave it out. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include <ps5/klog.h>
#include <ps5platform/kernel.h>

#define GIB (1ull << 30)
#define MIN_FIXED_ADDRESS 0x1000000000ull /* 64 GiB */
#define USER_TOP 0x800000000000ull /* FreeBSD amd64 user limit; unverified on the console */
#define HOLD_SECONDS 30
#define SHORT_HOLD_SECONDS 10
#define MAX_VMMAP 4096

enum { RESERVE, RESERVE_FIXED, MMAP_NONE, MMAP_GUARD, METHODS };
static const char *const method_name[METHODS] = {
    "reserve", "reserve-fixed", "mmap-prot-none", "mmap-guard"
};

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    klog_printf("VAPROBE-NATIVE %s\n", line);
}

#ifndef VAPROBE_NO_PTSTATS
int sceKernelGetPageTableStats(void *, void *, void *, void *); /* unverified */
#endif

static void stats(const char *when, int method, unsigned long long size)
{
    size_t flexible = 0;
    int32_t fr = sceKernelAvailableFlexibleMemorySize(&flexible);
#ifndef VAPROBE_NO_PTSTATS
    static uint32_t buffer[4][64];
    int r;
    memset(buffer, 0, sizeof(buffer));
    r = sceKernelGetPageTableStats(buffer[0], buffer[1], buffer[2], buffer[3]);
    say("ptstats when=%s method=%s size=0x%llx ret=0x%08x a=%u,%u,%u,%u b=%u,%u,%u,%u "
        "c=%u,%u,%u,%u d=%u,%u,%u,%u flexible-ret=0x%08x flexible=0x%zx",
        when, method_name[method], size, (unsigned)r,
        buffer[0][0], buffer[0][1], buffer[0][2], buffer[0][3],
        buffer[1][0], buffer[1][1], buffer[1][2], buffer[1][3],
        buffer[2][0], buffer[2][1], buffer[2][2], buffer[2][3],
        buffer[3][0], buffer[3][1], buffer[3][2], buffer[3][3], (unsigned)fr, flexible);
#else
    say("ptstats when=%s method=%s size=0x%llx flexible-ret=0x%08x flexible=0x%zx",
        when, method_name[method], size, (unsigned)fr, flexible);
#endif
}

/* Reads this process's mappings, in address order, into ranges; returns the
 * count, or -1 when the map cannot be read. */
static int read_vmmap(uint64_t (*ranges)[2], int max)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid() };
    size_t length = 0;
    char *buffer, *p;
    int n = 0;

    if (sysctl(mib, 4, NULL, &length, NULL, 0)) return -1;
    length = length * 4 / 3 + 4096;
    if (!(buffer = malloc(length))) return -1;
    if (sysctl(mib, 4, buffer, &length, NULL, 0))
    {
        free(buffer);
        return -1;
    }
    for (p = buffer; p < buffer + length && n < max;)
    {
        const struct kinfo_vmentry *entry = (const struct kinfo_vmentry *)p;
        if (entry->kve_structsize <= 0) break;
        ranges[n][0] = entry->kve_start;
        ranges[n][1] = entry->kve_end;
        n++;
        p += entry->kve_structsize;
    }
    free(buffer);
    return n;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* Lowest address >= floor, aligned to alignment, where size fits below
 * USER_TOP without overlapping ranges; 0 when there is none. */
static uint64_t lowest_gap(uint64_t (*ranges)[2], int n, uint64_t floor, uint64_t size, uint64_t alignment)
{
    uint64_t candidate = align_up(floor, alignment);
    int i;
    for (i = 0; i < n; i++)
    {
        if (ranges[i][1] <= candidate) continue;
        if (ranges[i][0] >= candidate + size) break;
        candidate = align_up(ranges[i][1], alignment);
    }
    return candidate + size <= USER_TOP && candidate >= floor ? candidate : 0;
}

/* Chooses a base for a fixed reserve of size; fills mode and top (end of the
 * highest mapping). Returns 0 when nothing fits or the map is unreadable. */
static uint64_t pick_base(uint64_t size, const char **mode, uint64_t *top)
{
    static uint64_t ranges[MAX_VMMAP][2];
    uint64_t base;
    int n = read_vmmap(ranges, MAX_VMMAP), i;

    *top = 0;
    *mode = "vmmap-unreadable";
    if (n < 0) return 0;
    for (i = 0; i < n; i++)
        if (ranges[i][1] > *top) *top = ranges[i][1];
    base = align_up(*top > MIN_FIXED_ADDRESS ? *top : MIN_FIXED_ADDRESS, size);
    if (base + size <= USER_TOP && base + size > base) return *mode = "above-all", base;
    if ((base = lowest_gap(ranges, n, MIN_FIXED_ADDRESS, size, size))) return *mode = "gap-size-aligned", base;
    if ((base = lowest_gap(ranges, n, MIN_FIXED_ADDRESS, size, PS5_KERNEL_DIRECT_ALIGNMENT)))
        return *mode = "gap", base;
    *mode = "no-fit";
    return 0;
}

/* 1 when [start, start + size) overlaps a mapping of this process, 0 when it
 * is free, -1 when the map cannot be read. MAP_FIXED would replace a mapping. */
static int range_in_use(uint64_t start, uint64_t size)
{
    static uint64_t ranges[MAX_VMMAP][2];
    int n = read_vmmap(ranges, MAX_VMMAP), i;
    if (n < 0) return -1;
    for (i = 0; i < n; i++)
        if (ranges[i][0] < start + size && ranges[i][1] > start) return 1;
    return 0;
}

/* Takes the range; on failure fills why and returns 0. */
static int take(int method, unsigned long long size, void **placed, char *why, size_t why_size)
{
    void *p = NULL;
    const char *mode;
    uint64_t base, top;
    int32_t r;
    int busy;

    switch (method)
    {
    case RESERVE:
        if ((r = sceKernelReserveVirtualRange(&p, size, 0, PS5_KERNEL_DIRECT_ALIGNMENT)))
            return snprintf(why, why_size, "sce=0x%08x", (unsigned)r), 0;
        break;
    case RESERVE_FIXED:
        if (!(base = pick_base(size, &mode, &top)))
            return snprintf(why, why_size, "skipped=%s top=0x%llx errno=%d", mode,
                            (unsigned long long)top, errno), 0;
        if ((busy = range_in_use(base, size)))
            return snprintf(why, why_size, "skipped=%s base=0x%llx errno=%d",
                            busy > 0 ? "range-in-use" : "vmmap-unreadable", (unsigned long long)base,
                            busy > 0 ? 0 : errno), 0;
        say("place method=%s size=0x%llx base=0x%llx mode=%s top=0x%llx", method_name[method], size,
            (unsigned long long)base, mode, (unsigned long long)top);
        p = (void *)base;
        if ((r = sceKernelReserveVirtualRange(&p, size, PS5_KERNEL_MAP_FIXED, PS5_KERNEL_DIRECT_ALIGNMENT)))
            return snprintf(why, why_size, "sce=0x%08x base=0x%llx", (unsigned)r, (unsigned long long)base), 0;
        break;
    case MMAP_NONE:
        p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return snprintf(why, why_size, "errno=%d", errno), 0;
        break;
    case MMAP_GUARD: /* FreeBSD: PROT_NONE, fd -1, offset 0, no MAP_ANON/MAP_PRIVATE */
        p = mmap(NULL, size, PROT_NONE, MAP_GUARD, -1, 0);
        if (p == MAP_FAILED) return snprintf(why, why_size, "errno=%d", errno), 0;
        break;
    }
    *placed = p;
    return 1;
}

static void give(int method, void *p, unsigned long long size)
{
    int r = method <= RESERVE_FIXED ? sceKernelMunmap(p, size) : munmap(p, size);
    if (r) say("release-failed method=%s addr=%p size=0x%llx ret=0x%08x errno=%d",
               method_name[method], p, size, (unsigned)r, errno);
}

/* Holds a taken range between marker lines, logging stats midway. */
static void hold(int method, void *p, unsigned long long size, unsigned seconds)
{
    say("hold start method=%s size=0x%llx addr=%p seconds=%u", method_name[method], size, p, seconds);
    stats("during", method, size);
    sleep(seconds / 2);
    stats("during", method, size);
    sleep(seconds - seconds / 2);
    say("hold end method=%s size=0x%llx", method_name[method], size);
}

int main(void)
{
    static const unsigned gib[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
    static const unsigned base_sizes = 6; /* up to 32 GiB; larger only for reserve-fixed */
    unsigned long long largest[METHODS] = { 0 }, size;
    char why[128];
    void *p;
    int m;
    size_t i;

    say("start pid=%d MAP_GUARD=0x%x MAP_FIXED=0x%x reserve-alignment=0x%zx fixed-min=0x%llx user-top=0x%llx",
        getpid(), MAP_GUARD, PS5_KERNEL_MAP_FIXED, PS5_KERNEL_DIRECT_ALIGNMENT, MIN_FIXED_ADDRESS, USER_TOP);
    stats("start", RESERVE, 0);

    for (m = 0; m < METHODS; m++)
        for (i = 0; i < (m == RESERVE_FIXED ? sizeof(gib) / sizeof(gib[0]) : base_sizes); i++)
        {
            size = gib[i] * GIB;
            if (!take(m, size, &p, why, sizeof(why)))
            {
                say("try method=%s size=0x%llx gib=%u result=fail %s", method_name[m], size, gib[i], why);
                continue;
            }
            say("try method=%s size=0x%llx gib=%u result=ok addr=%p", method_name[m], size, gib[i], p);
            stats("held", m, size);
            if (i >= base_sizes) hold(m, p, size, SHORT_HOLD_SECONDS);
            give(m, p, size);
            largest[m] = size;
        }

    for (m = 0; m < METHODS; m++)
    {
        if (!largest[m])
        {
            say("hold skipped method=%s reason=no-success", method_name[m]);
            continue;
        }
        stats("before", m, largest[m]);
        if (!take(m, largest[m], &p, why, sizeof(why)))
        {
            say("hold retake-failed method=%s size=0x%llx %s", method_name[m], largest[m], why);
            continue;
        }
        hold(m, p, largest[m], largest[m] > 32 * GIB ? SHORT_HOLD_SECONDS : HOLD_SECONDS);
        give(m, p, largest[m]);
        stats("after", m, largest[m]);
    }
    say("done");
    return 0;
}
