/* The production title exit must end the app through the shell, never libc exit:
 * native libc exit SIGSYS-traps in the title's main process on the console. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FOREVER_PREFIX "/test/prefix"
enum { ENDED_LOADEXEC = 1, ENDED_LIBC_EXIT, ENDED_WAIT };

static jmp_buf ended;
static int log_shutdowns, supervisor_shutdowns, loadexecs, libc_exits;
static char loadexec_path[64];
static int output_started, output_write = -1, game_relaunch_pending, forever_prefix_init;
static _Atomic int output_done;
static pthread_t output_thread;
static int duplicate_channel[2] = {-1, -1};
static atomic_int forever_menu_exit_pending, forever_menu_relaunch_pending;

static void io_log(const char *format, ...) { (void)format; }
static void forever_addon_trash_shutdown(void) {}
static void forever_title_voice_shutdown(void) {}
static void pw_voice_shutdown(void) {}
static void pw_title_audio_close(void) {}
static void pw_title_log_shutdown(void) { log_shutdowns++; }
static void pw_supervisor_shutdown(void) { assert(log_shutdowns == 1); supervisor_shutdowns++; }
static int join_thread(pthread_t thread, void **result) { (void)thread; (void)result; return 0; }
static void pw_duplicate_bridge_close(int channel[2]) { (void)channel; }
static void forever_say(const char *text) { (void)text; }
static void b1_wait(void) { longjmp(ended, ENDED_WAIT); }
static void pw_maintenance_loadexec_game(const char *reason) { (void)reason; assert(0); }
static int forever_finish_init(const char *prefix, int status) { (void)prefix; (void)status; assert(0); return 0; }
static int forever_permission_error(int result) { (void)result; return 0; }
static void pw_forever_permission_stop(const char *path, const char *root) { (void)path; (void)root; }
static int forever_prefix_ready(const char *prefix) { (void)prefix; return 0; }
/* The shell ends the app on success: LoadExec("exit") does not return. */
static int sceSystemServiceLoadExec(const char *path, const char *const argv[])
{
    assert(!argv && supervisor_shutdowns == 1);
    snprintf(loadexec_path, sizeof(loadexec_path), "%s", path);
    loadexecs++;
    longjmp(ended, ENDED_LOADEXEC);
}
static int test_usleep(useconds_t microseconds) { (void)microseconds; assert(0); return 0; }
#define usleep test_usleep
static void __attribute__((noreturn, unused)) libc_exit(int status)
{
    (void)status;
    libc_exits++;
    longjmp(ended, ENDED_LIBC_EXIT);
}
#define exit(status) libc_exit(status)

#define PW_PARENT_ROLE 1
#define pw_native_exit launcher_exit
#define PW_RUNTIME_GPU 0
#define PW_RUNTIME_HEADLESS 0
#include "native-exit-function.inc"
#undef pw_native_exit
#undef PW_RUNTIME_GPU
#undef PW_RUNTIME_HEADLESS
#define pw_native_exit game_exit
#define PW_RUNTIME_GPU 1
#define PW_RUNTIME_HEADLESS 1
#include "native-exit-function.inc"
#undef pw_native_exit

static int end_title(void (*title_exit)(int), int status)
{
    log_shutdowns = supervisor_shutdowns = loadexecs = libc_exits = 0;
    loadexec_path[0] = 0;
    int how = setjmp(ended);
    if (!how) title_exit(status);
    return how;
}
int main(void)
{
    /* Wine program ended on its own: the Steam probe (0) and a failed installer. */
    static const int statuses[] = {0, (int)0xc0000018};
    for (unsigned i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
    {
        assert(end_title(launcher_exit, statuses[i]) == ENDED_LOADEXEC);
        assert(loadexecs == 1 && !libc_exits && !strcmp(loadexec_path, "exit"));
        assert(end_title(game_exit, statuses[i]) == ENDED_LOADEXEC);
        assert(loadexecs == 1 && !libc_exits && !strcmp(loadexec_path, "exit"));
    }
    atomic_store(&forever_menu_exit_pending, 1);
    assert(end_title(launcher_exit, 0) == ENDED_LOADEXEC && !strcmp(loadexec_path, "exit"));
    puts("native exit: launcher and game end through LoadExec(exit), never libc exit PASS");
    return 0;
}
