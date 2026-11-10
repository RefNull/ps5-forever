/* va-probe: measures large VirtualAlloc(MEM_RESERVE) reservations through
 * Wine's real path. Run it inside the title's Wine in place of an app. Every
 * line goes to stdout and is appended to C:\va-probe.txt, prefixed "VAPROBE ".
 * Optional argument: cap of the running-total phase in GiB, used only when it
 * is all digits (default 64), so Steam's arguments are ignored when it runs in
 * place of steam.exe. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KiB (1ull << 10)
#define MiB (1ull << 20)
#define GiB (1ull << 30)
#define STEP (256 * MiB)
#define HOLD_MS 30000
#define DEFAULT_CAP_GIB 64

static FILE *log_file;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printf("VAPROBE %s\n", line);
    fflush(stdout);
    if (log_file)
    {
        fprintf(log_file, "VAPROBE %s\n", line);
        fflush(log_file);
    }
}

static void *reserve(void *address, unsigned long long size, DWORD *error)
{
    void *p = VirtualAlloc(address, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    *error = p ? 0 : GetLastError();
    return p;
}

static void release(void *p)
{
    if (p && !VirtualFree(p, 0, MEM_RELEASE))
        say("free-failed addr=%p err=%lu", p, GetLastError());
}

/* One reserve-log-free attempt; returns whether it succeeded. */
static int attempt(const char *phase, unsigned long long size)
{
    DWORD error, started = GetTickCount();
    void *p = reserve(NULL, size, &error);
    say("%s size=0x%llx gib=%.2f result=%s addr=%p err=%lu ms=%lu", phase, size,
        (double)size / GiB, p ? "ok" : "fail", p, error, GetTickCount() - started);
    release(p);
    return p != NULL;
}

static void commit_test(unsigned long long size)
{
    DWORD error;
    unsigned char *base = reserve(NULL, size, &error), *middle, *c;
    size_t i;
    int good = 1;
    if (!base)
    {
        say("commit reserve size=0x%llx result=fail err=%lu", size, error);
        return;
    }
    middle = base + ((size / 2) & ~(64 * KiB - 1));
    c = VirtualAlloc(middle, 64 * KiB, MEM_COMMIT, PAGE_READWRITE);
    if (!c)
        say("commit reserve=%p size=0x%llx at=%p result=fail err=%lu", base, size, middle, GetLastError());
    else
    {
        for (i = 0; i < 64 * KiB; i++) c[i] = (unsigned char)(i * 7 + 1);
        for (i = 0; i < 64 * KiB; i++) good &= c[i] == (unsigned char)(i * 7 + 1);
        say("commit reserve=%p size=0x%llx at=%p result=ok write=%s", base, size, c,
            good ? "ok" : "mismatch");
    }
    release(base);
}

static void chromium_pattern(void)
{
    const unsigned long long half = 16 * GiB, outer = 32 * GiB - 64 * KiB;
    DWORD error, error2;
    char *p = reserve(NULL, outer, &error), *aligned, *q;
    say("chromium outer size=0x%llx result=%s addr=%p err=%lu", outer, p ? "ok" : "fail", p, error);
    if (p)
    {
        release(p);
        aligned = (char *)(((unsigned long long)p + half - 1) & ~(half - 1));
        q = reserve(aligned, half, &error);
        say("chromium aligned want=%p size=0x%llx result=%s addr=%p err=%lu", aligned, half,
            q ? (q == aligned ? "ok" : "moved") : "fail", q, error);
        release(q);
    }
    p = reserve(NULL, half, &error);
    q = reserve(NULL, half, &error2);
    say("chromium two-calls size=0x%llx first=%s addr=%p err=%lu second=%s addr=%p err=%lu", half,
        p ? "ok" : "fail", p, error, q ? "ok" : "fail", q, error2);
    release(q);
    release(p);
}

/* V8's sandbox reserves up to 1 TiB per renderer, with smaller fallbacks. */
static void v8_pattern(void)
{
    static const unsigned long long gib[] = { 1024, 512, 256, 128, 64 };
    unsigned long long got = 0;
    size_t i;
    for (i = 0; i < sizeof(gib) / sizeof(gib[0]) && !got; i++)
        if (attempt("v8", gib[i] * GiB)) got = gib[i] * GiB;
    say("v8 largest=0x%llx gib=%llu", got, got / GiB);
}

static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void running_total(unsigned long long cap_gib)
{
    void **chunks = calloc(cap_gib ? cap_gib : 1, sizeof(*chunks));
    unsigned long long n = 0, i;
    DWORD error = 0;
    if (!chunks) return;
    while (n < cap_gib)
    {
        void *p = reserve(NULL, GiB, &error);
        if (!p) break;
        chunks[n++] = p;
        say("total chunk=%llu addr=%p", n, p);
    }
    say("total reserved=0x%llx gib=%llu stop=%s err=%lu", n * GiB, n, n < cap_gib ? "fail" : "cap", error);
    say("hold start total=0x%llx gib=%llu", n * GiB, n);
    Sleep(HOLD_MS);
    say("hold end");
    for (i = 0; i < n; i++) release(chunks[i]);
    say("total freed=%llu", n);
    free(chunks);
}

int main(int argc, char **argv)
{
    static const unsigned long long gib[] = { 1, 2, 4, 8, 12, 16, 24, 32 };
    unsigned long long cap = argc > 1 && all_digits(argv[1]) ? strtoull(argv[1], NULL, 10) : DEFAULT_CAP_GIB;
    unsigned long long lo = 0, hi = 1024, mid;
    MEMORYSTATUSEX status;
    SYSTEM_INFO info;
    size_t i;

    log_file = fopen("C:\\va-probe.txt", "a");
    memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(status);
    GetSystemInfo(&info);
    GlobalMemoryStatusEx(&status);
    say("args argc=%d cap-gib=%llu", argc, cap);
    say("start pid=%lu min-app=%p max-app=%p total-virtual=0x%llx avail-virtual=0x%llx",
        GetCurrentProcessId(), info.lpMinimumApplicationAddress, info.lpMaximumApplicationAddress,
        status.ullTotalVirtual, status.ullAvailVirtual);

    for (i = 0; i < sizeof(gib) / sizeof(gib[0]); i++) attempt("single", gib[i] * GiB);
    attempt("single", 0x7ffff0000ull);

    /* Largest single reservation, in 256 MiB steps, up to 256 GiB. */
    if (attempt("search", hi * STEP)) lo = hi;
    else
        while (hi - lo > 1)
        {
            mid = (lo + hi) / 2;
            if (attempt("search", mid * STEP)) lo = mid; else hi = mid;
        }
    say("search largest=0x%llx gib=%.2f%s", lo * STEP, (double)(lo * STEP) / GiB,
        lo == 1024 ? " bound=reached" : "");
    if (lo) commit_test(lo * STEP);

    chromium_pattern();
    v8_pattern();
    running_total(cap);
    say("done");
    if (log_file) fclose(log_file);
    return 0;
}
