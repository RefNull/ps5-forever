/* fs-probe: measures file-operation latency inside the title, where the
 * console throttles a busy title's file operations. Run it inside the title's
 * Wine in place of an app; arguments are ignored. Every line goes to stdout
 * and is appended to C:\fs-probe.txt, prefixed "FSPROBE ".
 *
 * Phases, in C:\fs-probe\<tick>\:
 *   idle       200 cycles of create, write 4 KiB, close, rename, delete
 *   pressure   one scratch file written in 8 MiB chunks up to 2 GiB, then
 *              deleted; stops early when a chunk takes over 2 s
 *   throttled  the idle cycles again, immediately after
 *   mkdir      50 CreateDirectoryW/RemoveDirectoryW pairs
 * Each operation is timed separately; per operation the phase logs median,
 * p90, max and the count over 300 ms. Everything it creates is removed. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CYCLES 200
#define DIRS 50
#define SMALL_SIZE 4096
#define CHUNK (8u << 20)
#define PRESSURE_MIB 2048u
#define REPORT_MIB 128u
#define STALL_MS 2000.0
#define SLOW_MS 300.0

static FILE *log_file;
static LARGE_INTEGER frequency;
static WCHAR root[MAX_PATH], run[MAX_PATH];

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printf("FSPROBE %s\n", line);
    fflush(stdout);
    if (log_file)
    {
        fprintf(log_file, "FSPROBE %s\n", line);
        fflush(log_file);
    }
}

static LONGLONG now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double ms_since(LONGLONG start)
{
    return (double)(now() - start) * 1000.0 / (double)frequency.QuadPart;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Logs median, p90, max and the count over SLOW_MS of n samples (sorted in place). */
static void report(const char *phase, const char *op, double *ms, int n, int failed)
{
    int i, slow = 0;
    double median, p90, max;
    if (!n)
    {
        say("%s op=%s n=0 failed=%d", phase, op, failed);
        return;
    }
    qsort(ms, n, sizeof(*ms), compare);
    for (i = 0; i < n; i++) slow += ms[i] > SLOW_MS;
    median = n % 2 ? ms[n / 2] : (ms[n / 2 - 1] + ms[n / 2]) / 2;
    p90 = ms[(n * 9 + 9) / 10 - 1];
    max = ms[n - 1];
    say("%s op=%s n=%d failed=%d median-ms=%.3f p90-ms=%.3f max-ms=%.3f over300=%d", phase, op, n,
        failed, median, p90, max, slow);
}

enum { OP_CREATE, OP_WRITE, OP_CLOSE, OP_RENAME, OP_DELETE, OPS };
static const char *const op_name[OPS] = { "create", "write", "close", "rename", "delete" };

static void cycles(const char *phase)
{
    static double ms[OPS][CYCLES];
    static char data[SMALL_SIZE];
    int n[OPS] = { 0 }, failed[OPS] = { 0 }, i, op;
    WCHAR a[MAX_PATH], b[MAX_PATH];
    LONGLONG started = now(), t;
    HANDLE h;
    DWORD written;
    BOOL ok;

    memset(data, 0x5a, sizeof(data));
    for (i = 0; i < CYCLES; i++)
    {
        _snwprintf(a, MAX_PATH, L"%ls\\%hs-%d.a", run, phase, i);
        _snwprintf(b, MAX_PATH, L"%ls\\%hs-%d.b", run, phase, i);

        t = now();
        h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            if (!failed[OP_CREATE]++) say("%s op=create cycle=%d err=%lu", phase, i, GetLastError());
            continue;
        }
        ms[OP_CREATE][n[OP_CREATE]++] = ms_since(t);

        t = now();
        ok = WriteFile(h, data, sizeof(data), &written, NULL) && written == sizeof(data);
        if (ok) ms[OP_WRITE][n[OP_WRITE]++] = ms_since(t);
        else if (!failed[OP_WRITE]++) say("%s op=write cycle=%d err=%lu", phase, i, GetLastError());

        t = now();
        if (CloseHandle(h)) ms[OP_CLOSE][n[OP_CLOSE]++] = ms_since(t);
        else if (!failed[OP_CLOSE]++) say("%s op=close cycle=%d err=%lu", phase, i, GetLastError());

        t = now();
        if (MoveFileExW(a, b, 0)) ms[OP_RENAME][n[OP_RENAME]++] = ms_since(t);
        else
        {
            if (!failed[OP_RENAME]++) say("%s op=rename cycle=%d err=%lu", phase, i, GetLastError());
            wcscpy(b, a);
        }

        t = now();
        if (DeleteFileW(b)) ms[OP_DELETE][n[OP_DELETE]++] = ms_since(t);
        else if (!failed[OP_DELETE]++) say("%s op=delete cycle=%d err=%lu", phase, i, GetLastError());
    }
    for (op = 0; op < OPS; op++) report(phase, op_name[op], ms[op], n[op], failed[op]);
    say("%s cycles=%d total-ms=%.1f", phase, CYCLES, ms_since(started));
}

static void pressure(void)
{
    WCHAR path[MAX_PATH];
    LONGLONG started = now(), t;
    unsigned mib = 0, stalled = 0;
    double chunk_ms, worst = 0, total_ms;
    DWORD written;
    char *data;
    HANDLE h;

    _snwprintf(path, MAX_PATH, L"%ls\\pressure.bin", run);
    if (!(data = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
    {
        say("pressure alloc-failed err=%lu", GetLastError());
        return;
    }
    memset(data, 0xa5, CHUNK);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        say("pressure create-failed err=%lu", GetLastError());
        VirtualFree(data, 0, MEM_RELEASE);
        return;
    }
    while (mib < PRESSURE_MIB)
    {
        t = now();
        if (!WriteFile(h, data, CHUNK, &written, NULL) || written != CHUNK)
        {
            say("pressure write-failed at=%u err=%lu", mib, GetLastError());
            break;
        }
        chunk_ms = ms_since(t);
        mib += CHUNK >> 20;
        if (chunk_ms > worst) worst = chunk_ms;
        if (mib % REPORT_MIB == 0)
        {
            say("pressure mib=%u chunk-ms=%.3f worst-ms=%.3f elapsed-ms=%.1f", mib, chunk_ms, worst,
                ms_since(started));
            worst = 0;
        }
        if (chunk_ms > STALL_MS)
        {
            say("throttled at=%u chunk-ms=%.3f", mib, chunk_ms);
            stalled = 1;
            break;
        }
    }
    total_ms = ms_since(started);
    say("pressure written-mib=%u ms=%.1f mib-per-s=%.1f stopped=%s", mib, total_ms,
        total_ms > 0 ? mib * 1000.0 / total_ms : 0.0, stalled ? "throttled" : mib < PRESSURE_MIB ? "error" : "cap");
    t = now();
    CloseHandle(h);
    say("pressure close-ms=%.3f", ms_since(t));
    t = now();
    if (DeleteFileW(path)) say("pressure delete-ms=%.3f", ms_since(t));
    else say("pressure delete-failed err=%lu", GetLastError());
    VirtualFree(data, 0, MEM_RELEASE);
}

static void directories(void)
{
    static double made[DIRS], removed[DIRS];
    int n_made = 0, n_removed = 0, failed_made = 0, failed_removed = 0, i;
    WCHAR path[MAX_PATH];
    LONGLONG t;

    for (i = 0; i < DIRS; i++)
    {
        _snwprintf(path, MAX_PATH, L"%ls\\dir-%d", run, i);
        t = now();
        if (!CreateDirectoryW(path, NULL))
        {
            if (!failed_made++) say("mkdir op=mkdir pair=%d err=%lu", i, GetLastError());
            continue;
        }
        made[n_made++] = ms_since(t);
        t = now();
        if (RemoveDirectoryW(path)) removed[n_removed++] = ms_since(t);
        else if (!failed_removed++) say("mkdir op=rmdir pair=%d err=%lu", i, GetLastError());
    }
    report("mkdir", "mkdir", made, n_made, failed_made);
    report("mkdir", "rmdir", removed, n_removed, failed_removed);
}

/* Removes any file or folder a failed step left in the run folder, then the
 * run folder, then the root if it is empty. */
static void clean_up(void)
{
    WIN32_FIND_DATAW found;
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    HANDLE search;
    int left = 0;

    _snwprintf(pattern, MAX_PATH, L"%ls\\*", run);
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            _snwprintf(path, MAX_PATH, L"%ls\\%ls", run, found.cFileName);
            left++;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path);
            else DeleteFileW(path);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (!RemoveDirectoryW(run)) say("cleanup run-folder err=%lu", GetLastError());
    RemoveDirectoryW(root); /* fails harmlessly when another run's folder remains */
    say("cleanup leftovers=%d", left);
}

int main(void)
{
    ULONGLONG tick = GetTickCount64();

    log_file = fopen("C:\\fs-probe.txt", "a");
    QueryPerformanceFrequency(&frequency);
    wcscpy(root, L"C:\\fs-probe");
    _snwprintf(run, MAX_PATH, L"%ls\\%llu", root, tick);
    say("start pid=%lu tick=%llu qpc-hz=%lld", GetCurrentProcessId(), tick, frequency.QuadPart);
    if (!CreateDirectoryW(root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        say("root mkdir-failed err=%lu", GetLastError());
    if (!CreateDirectoryW(run, NULL))
    {
        say("run mkdir-failed err=%lu", GetLastError());
        say("done");
        return 1;
    }

    cycles("idle");
    pressure();
    cycles("throttled");
    directories();
    clean_up();
    say("done");
    if (log_file) fclose(log_file);
    return 0;
}
