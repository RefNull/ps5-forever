/* ntdll's direct-view reservation fallback against a mocked kernel: a
 * reservation the kernel places itself fails with ENOMEM from 16 GiB up, one
 * at a fixed address succeeds where the VM map is free. The address space is
 * a real PROT_NONE host reservation, so committed units can be written. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int BOOL;
typedef unsigned char BYTE;
typedef uintptr_t UINT_PTR;
#define TRUE 1
#define FALSE 0
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#define GIB ((size_t)1 << 30)

#define PS5_KERNEL_PROT_CPU_READ 0x01
#define PS5_KERNEL_PROT_CPU_WRITE 0x02
#define PS5_KERNEL_PROT_CPU_EXEC 0x04
#define PS5_KERNEL_MAP_FIXED 0x10
#define PS5_KERNEL_DIRECT_TYPE_CPU 12
#define PS5_KERNEL_PAGE_SIZE ((size_t)0x4000)
#define PS5_KERNEL_DIRECT_ALIGNMENT ((size_t)0x10000)
#define KERNEL_ENOMEM ((int32_t)0x8002000c)

/* The process's VM map as the mocked kernel keeps it. */
static struct { char *start, *end; } map[64];
static unsigned map_count;
static char *dirty_low, *dirty_high;
static unsigned fixed_calls, refused_calls, vmmap_reads, physical_live, notes;
static char log_text[4096];
static unsigned fallback_lines;

static BOOL map_overlaps( char *start, size_t size )
{
    unsigned i;
    for (i = 0; i < map_count; i++)
        if (map[i].start < start + size && map[i].end > start) return TRUE;
    return FALSE;
}

static void map_add( char *start, size_t size )
{
    assert( map_count < sizeof(map) / sizeof(map[0]) && !map_overlaps( start, size ) );
    map[map_count].start = start;
    map[map_count++].end = start + size;
}

static void map_remove( char *start, size_t size )
{
    char *end = start + size;
    unsigned i;

    for (i = 0; i < map_count; i++)
    {
        if (map[i].end <= start || map[i].start >= end) continue;
        if (map[i].start < start && map[i].end > end)
        {
            char *tail = map[i].end;
            map[i].end = start;
            map_add( end, tail - end );
        }
        else if (map[i].start < start) map[i].end = start;
        else if (map[i].end > end) map[i].start = end;
        else
        {
            map[i--] = map[--map_count];
        }
    }
}

static BOOL ps5_title_startup_active;
enum ps5_title_host_call { PS5_TITLE_HOST_MAP, PS5_TITLE_HOST_PROTECT, PS5_TITLE_HOST_UNMAP,
                           PS5_TITLE_HOST_RESERVE };
static void ps5_title_startup_host_call( enum ps5_title_host_call op, void *addr, size_t size,
                                         int prot, BOOL success, uint64_t elapsed )
{
    (void)op; (void)addr; (void)size; (void)prot; (void)success; (void)elapsed;
    assert( !"startup profiling is off" );
}
static uint64_t ps5_title_profile_now_ns(void) { return 0; }
static int title_profile_is_enabled(void) { return 0; }
static int __wine_ps5_is_title(void) { return 1; }
static const char *ps5_title_drive( char drive ) { (void)drive; return NULL; }
static BOOL virtual_title_direct_cache_trim(void) { return FALSE; }
static void *anon_mmap_alloc( size_t size, int prot )
{
    return mmap( NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0 );
}

static int sceKernelDebugOutText( int channel, const char *text )
{
    assert( !channel );
    if (strstr( text, "reserve fallback" )) fallback_lines++;
    strncat( log_text, text, sizeof(log_text) - strlen( log_text ) - 1 );
    return 0;
}

static int32_t sceKernelReserveVirtualRange( void **addr, size_t size, int flags, size_t alignment )
{
    char *hint = *addr;
    unsigned i;

    assert( alignment == PS5_KERNEL_DIRECT_ALIGNMENT && size && !(size % alignment) );
    if (flags == PS5_KERNEL_MAP_FIXED)
    {
        assert( !map_overlaps( hint, size ) );  /* a fixed reservation would replace it */
        fixed_calls++;
        map_add( hint, size );
        return 0;
    }
    assert( !flags );
    if (size >= 16 * GIB)
    {
        refused_calls++;
        return KERNEL_ENOMEM;
    }
    if (!hint || map_overlaps( hint, size ))
        for (hint = (char *)0x10000;; hint += alignment)  /* elsewhere, as the kernel would */
        {
            for (i = 0; i < map_count; i++)
                if (map[i].start < hint + size && map[i].end > hint) break;
            if (i == map_count) break;
        }
    map_add( hint, size );
    *addr = hint;
    return 0;
}

static int32_t sceKernelMunmap( void *addr, size_t size )
{
    char *start = addr;

    map_remove( start, size );
    if (start < dirty_high && start + size > dirty_low)
        assert( mmap( start, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0 ) == start );
    return 0;
}

static int host_prot( int prot )
{
    return ((prot & PS5_KERNEL_PROT_CPU_READ) ? PROT_READ : 0) |
           ((prot & PS5_KERNEL_PROT_CPU_WRITE) ? PROT_WRITE : 0);
}

static int32_t sceKernelMprotect( const void *addr, size_t size, int prot )
{
    assert( map_overlaps( (char *)addr, size ) );
    return mprotect( (void *)addr, size, host_prot( prot ) ) ? -1 : 0;
}

static int32_t sceKernelAllocateDirectMemory( int64_t low, int64_t high, size_t size, size_t alignment,
                                              int type, int64_t *physical )
{
    static int64_t next;
    (void)low; (void)high;
    assert( size == PS5_KERNEL_DIRECT_ALIGNMENT && alignment == size && type == PS5_KERNEL_DIRECT_TYPE_CPU );
    physical_live++;
    *physical = next += size;
    return 0;
}

static int32_t sceKernelReleaseDirectMemory( int64_t start, size_t size )
{
    assert( start > 0 && size == PS5_KERNEL_DIRECT_ALIGNMENT && physical_live );
    physical_live--;
    return 0;
}

static int32_t sceKernelMapDirectMemory( void **addr, size_t size, int prot, int flags,
                                         int64_t physical, size_t alignment )
{
    char *start = *addr;

    assert( flags == PS5_KERNEL_MAP_FIXED && physical > 0 && alignment == PS5_KERNEL_PAGE_SIZE );
    assert( map_overlaps( start, size ) );  /* replaces the unit's reservation */
    map_remove( start, size );
    map_add( start, size );
    if (!dirty_low || start < dirty_low) dirty_low = start;
    if (start + size > dirty_high) dirty_high = start + size;
    return mprotect( start, size, host_prot( prot ) ) ? -1 : 0;
}

static int32_t sceKernelAvailableDirectMemorySize( int64_t low, int64_t high, size_t alignment,
                                                   int64_t *physical, size_t *available )
{
    (void)low; (void)high; (void)alignment;
    *physical = 0;
    *available = 0;
    return 0;
}

#include "wine-direct-reserve-core.inc"

/* virtual.c's side: views, GPU windows, the preloader range, the VM map. */
#define VPROT_COMMITTED 0x20
#define VPROT_PS5_DIRECT 0x2000
#define ROUND_ADDR(addr, mask) ((void *)((UINT_PTR)(addr) & ~(UINT_PTR)(mask)))
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_VMMAP 32
struct kinfo_vmentry { int kve_structsize; uint64_t kve_start, kve_end; };
struct file_view { void *base; size_t size; };
static struct file_view views[8];
static unsigned view_count;
static BOOL ps5_title_radv_ranges = TRUE;
static uint64_t radv_low, radv_high;
static void *preload_reserve_start, *preload_reserve_end;

static struct file_view *find_view_range( const void *addr, size_t size )
{
    unsigned i;
    for (i = 0; i < view_count; i++)
        if ((const char *)views[i].base < (const char *)addr + size &&
            (const char *)views[i].base + views[i].size > (const char *)addr) return &views[i];
    return NULL;
}

static int wine_ps5_title_radv_overlap( uint64_t start, uint64_t size, uint64_t *low, uint64_t *high )
{
    if (start >= radv_high || start + size <= radv_low) return 0;
    if (low) *low = radv_low;
    if (high) *high = radv_high;
    return 1;
}

static void ps5_title_note_reservation( void *base, size_t size )
{
    assert( !wine_ps5_title_radv_overlap( (UINT_PTR)base, size, NULL, NULL ) );
    notes++;
}

static int test_sysctl( const int *mib, unsigned int count, void *buffer, size_t *length,
                        const void *new_value, size_t new_length )
{
    struct kinfo_vmentry *entries = buffer;
    unsigned i;

    assert( count == 4 && mib[0] == CTL_KERN && mib[1] == KERN_PROC && mib[2] == KERN_PROC_VMMAP );
    assert( mib[3] == getpid() && !new_value && !new_length );
    if (!buffer)
    {
        *length = map_count * sizeof(*entries);
        return 0;
    }
    vmmap_reads++;
    assert( *length >= map_count * sizeof(*entries) );
    for (i = 0; i < map_count; i++)
    {
        entries[i].kve_structsize = sizeof(*entries);
        entries[i].kve_start = (UINT_PTR)map[i].start;
        entries[i].kve_end = (UINT_PTR)map[i].end;
    }
    *length = map_count * sizeof(*entries);
    return 0;
}
#define sysctl test_sysctl

#include "wine-direct-reserve-virtual.inc"

static struct ps5_direct_view *add_view( char *base, size_t size )
{
    struct ps5_direct_view *view = ps5_direct_create_view( base, size );

    assert( view );
    views[view_count].base = base;
    views[view_count++].size = size;
    return view;
}

static void release_view( struct ps5_direct_view *view )
{
    char *base = view->base;
    size_t size = view->units * DIRECT_UNIT_SIZE;

    assert( ps5_direct_release_whole( view ) );
    ps5_direct_free_view( view );
    assert( views[view_count - 1].base == base );
    view_count--;
    assert( !map_overlaps( base, size ) );
}

int main(void)
{
    char *host, *arena, *chromium, *small;
    size_t cage = 32 * GIB - DIRECT_UNIT_SIZE, unit = DIRECT_UNIT_SIZE, i;
    unsigned fixed, reads;
    char expect[128];
    struct ps5_direct_view *view;

    /* Back to back at the top, so that each check alone decides one step: a
     * Wine view, a GPU window, the preloader's range and a native mapping
     * only the VM map shows. Nothing below them is taken. */
    host = mmap( NULL, 80 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    assert( host != MAP_FAILED );
    arena = (char *)(((UINT_PTR)host + 16 * GIB - 1) & ~(UINT_PTR)(16 * GIB - 1));
    direct_memory_enabled = 1;
    direct_memory_size = (int64_t)1 << 40;
    views[view_count].base = arena + 63 * GIB;
    views[view_count++].size = GIB;
    radv_low = (UINT_PTR)(arena + 62 * GIB);
    radv_high = (UINT_PTR)(arena + 63 * GIB);
    preload_reserve_start = arena + 61 * GIB;
    preload_reserve_end = arena + 62 * GIB;
    map_add( arena + 60 * GIB, GIB );

    /* A committed allocation never takes the fallback. */
    assert( !title_direct_reserve_high( arena, arena + 64 * GIB, cage, unit - 1,
                                        VPROT_PS5_DIRECT | VPROT_COMMITTED ) );
    assert( title_direct_reserve( arena + 16 * GIB, 16 * GIB, VPROT_PS5_DIRECT | VPROT_COMMITTED ) == MAP_FAILED &&
            errno == ENOMEM );
    assert( !fixed_calls && !vmmap_reads && !fallback_lines && refused_calls == 1 );

    /* The search steps below each obstacle in turn. */
    assert( title_direct_reserve_high( arena + 56 * GIB, arena + 64 * GIB, GIB, unit - 1, VPROT_PS5_DIRECT ) ==
            arena + 59 * GIB );
    assert( fixed_calls == 1 && vmmap_reads == 2 && fallback_lines == 1 && notes == 1 );
    assert( !sceKernelMunmap( arena + 59 * GIB, GIB ) );

    /* 1. Chromium reserves 32 GiB - 64 KiB: the ordinary placement is
     * refused; the fallback steps below every obstacle above. */
    assert( ps5_direct_reserve( arena + GIB, cage ) == MAP_FAILED && errno == ENOMEM );
    assert( refused_calls == 2 );
    assert( title_direct_reserve_high( arena, arena + 64 * GIB, cage, unit - 1, VPROT_PS5_DIRECT ) ==
            arena + 28 * GIB + unit );
    assert( fixed_calls == 2 && vmmap_reads == 4 && fallback_lines == 2 && notes == 2 );
    snprintf( expect, sizeof(expect), "wine: ps5-direct reserve fallback size=0x7ffff0000 addr=0x%llx\n",
              (unsigned long long)(UINT_PTR)(arena + 28 * GIB + unit) );
    assert( strstr( log_text, expect ) );
    view = add_view( arena + 28 * GIB + unit, cage );

    /* 2. It frees it and re-reserves an aligned 16 GiB inside it. */
    release_view( view );
    chromium = arena + 32 * GIB;
    assert( title_direct_reserve( chromium, 16 * GIB, VPROT_PS5_DIRECT ) == chromium );
    assert( refused_calls == 3 && fixed_calls == 3 && fallback_lines == 3 );
    view = add_view( chromium, 16 * GIB );
    /* A base whose range the VM map shows taken is never reserved over. */
    fixed = fixed_calls;
    assert( title_direct_reserve( arena + 48 * GIB, 16 * GIB, VPROT_PS5_DIRECT ) == MAP_FAILED &&
            errno == EEXIST && fixed_calls == fixed );

    /* 3. Commit, write, decommit one unit, release. */
    assert( ps5_direct_ensure_backed( view, chromium + GIB, 4 * unit ) );
    assert( ps5_direct_finish_commit( view, chromium + GIB, 4 * unit, FALSE ) );
    assert( physical_live == 4 );
    assert( !ps5_direct_protect( view, chromium + GIB, 4 * unit, PROT_READ | PROT_WRITE ) );
    for (i = 0; i < 4 * unit; i++) assert( !chromium[GIB + i] );
    memset( chromium + GIB, 0x5a, 4 * unit );
    for (i = 0; i < 4 * unit; i++) assert( chromium[GIB + i] == 0x5a );
    assert( ps5_direct_drop_unit( view, chromium + GIB, TRUE ) );
    assert( !ps5_direct_is_backed( view, chromium + GIB ) && ps5_direct_is_backed( view, chromium + GIB + unit ) );
    assert( physical_live == 3 && chromium[GIB + unit] == 0x5a );
    release_view( view );
    assert( !physical_live && map_count == 1 && fallback_lines == 3 );

    /* A zero-bits limit and an alignment are honoured. */
    assert( title_direct_reserve_high( arena, arena + 20 * GIB, 16 * GIB, 4 * GIB - 1, VPROT_PS5_DIRECT ) ==
            arena + 4 * GIB );
    assert( !title_direct_reserve_high( arena, arena + 20 * GIB, cage, unit - 1, VPROT_PS5_DIRECT ) );
    assert( sceKernelMunmap( arena + 4 * GIB, 16 * GIB ) == 0 );

    /* 4. A small reservation takes the ordinary path, never the fallback. */
    fixed = fixed_calls;
    reads = vmmap_reads;
    small = ps5_direct_reserve( arena + GIB, 16 * unit );
    assert( small == arena + GIB );
    assert( title_direct_reserve( arena + 2 * GIB, 16 * unit, VPROT_PS5_DIRECT ) == arena + 2 * GIB );
    assert( fixed_calls == fixed && vmmap_reads == reads && fallback_lines == 4 );
    assert( !sceKernelMunmap( small, 16 * unit ) && !sceKernelMunmap( arena + 2 * GIB, 16 * unit ) );
    assert( map_count == 1 );

    munmap( host, 80 * GIB );
    puts( "DIRECT_RESERVE_HOST_PASS fallback_32g=1 aligned_rereserve_16g=1 commit_release=1 small_ordinary=1" );
    return 0;
}
