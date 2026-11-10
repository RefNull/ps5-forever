/* Exercise the production admission gate with deterministic child EOF/clock boundaries. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "protocol.h"
#include "../../title/forever-menu.h"

#define PW_RUNTIME_HEADLESS 0
#define PW_RUNTIME_GPU 0
struct wine_ps5_title_spawn { char *const *argv; unsigned windows_pid; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct pw_slot slots[PW_CHILDREN];
/* All but the last slot held; the AddonForge helper holds the highest of them. */
#define HELD (PW_CHILDREN - 1)
#define HELPER (PW_CHILDREN - 2)
_Static_assert(PW_CHILDREN >= 3, "the reserve needs a helper slot and a free slot");
static uint64_t generation = 7, addonforge_nonce;
static int stopping, stop_calls, kill_calls, reap_after_kill, stop_during_wait;
static int64_t clock_ms, graceful_eof, forced_eof;
static int addon_open;
static pthread_mutex_t message_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct forever_addon_snapshot pending_addon_snapshot;
static int pending_addon_kind;
static char pending_addon_error[FOREVER_MENU_STATUS_BYTES];
#define addon_notice pending_addon_error
static int64_t admission_now_ms(void) { return clock_ms; }
/* The transport object retains its real clock; only the extracted gate uses this one. */
#define io_now_ms admission_now_ms
static char last_log[256];
static void admission_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
}
#define io_log admission_log
int sceKernelDebugOutText(int channel, const char *text)
{
    (void)channel;
    (void)text;
    return 0;
}

static int forever_title_addons_action(enum forever_menu_command command, unsigned channel,
                                       enum forever_addon_tab tab, const char *key)
{
    assert(command == FOREVER_MENU_COMMAND_ADDONS_CLOSE && !channel &&
           tab == FOREVER_ADDON_INSTALLED && !key);
    /* Stopping the Windows helper must not run while the supervisor lock is held. */
    assert(!pthread_mutex_trylock(&lock));
    pthread_mutex_unlock(&lock);
    addon_open = 0;
    stop_calls++;
    return 0;
}
static int kill_slot(struct pw_slot *slot)
{
    assert(slot == &slots[HELPER] && slot->nonce == addonforge_nonce);
    slot->kill_requested = 1;
    kill_calls++;
    if (reap_after_kill) forced_eof = clock_ms + 10;
    return 0;
}
static int admission_sleep(const struct timespec *pause, struct timespec *remaining)
{
    assert(!remaining && pause->tv_sec == 0 && pause->tv_nsec == 10000000);
    assert(!pthread_mutex_trylock(&lock));
    clock_ms += 10;
    if (stop_during_wait) stopping = 1;
    if ((graceful_eof && clock_ms >= graceful_eof) ||
        (forced_eof && clock_ms >= forced_eof))
    {
        assert(!pw_slot_release(&slots[HELPER], generation, addonforge_nonce));
        graceful_eof = forced_eof = 0;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
#define nanosleep admission_sleep
#include "supervisor-admission.inc"
#undef nanosleep

static void fixture(unsigned occupied, int helper)
{
    memset(slots, 0, sizeof(slots));
    for (unsigned i = 0; i < occupied; i++)
        assert(pw_slot_reserve(slots, generation, i + 10, i + 100) == (int)i);
    addonforge_nonce = helper ? slots[HELPER].nonce : 0;
    stopping = stop_calls = kill_calls = reap_after_kill = stop_during_wait = 0;
    clock_ms = graceful_eof = forced_eof = 0;
    pending_addon_kind = 0;
    addon_open = 1;
    addon_notice[0] = 0;
}
static int admit(char *const argv[])
{
    struct wine_ps5_title_spawn spawn = {.argv = argv, .windows_pid = 200};
    pthread_mutex_lock(&lock);
    int index = reserve_spawn_slot(&spawn, 1000);
    pthread_mutex_unlock(&lock);
    return index;
}

static void pause_notice_consumer(void)
{
    static struct forever_menu_state menu;
    static const enum forever_menu_page pages[] = {
        FOREVER_MENU_WAGO_KEY, FOREVER_MENU_ADDON_CONFIRM, FOREVER_MENU_ADDON_PAGE};
    char text[FOREVER_MENU_TEXT_BYTES];
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
    {
        forever_menu_init(&menu);
        menu.page = pages[i];
        assert(pending_addon_kind == 3);
        title_desktop_addons_snapshot(&pending_addon_snapshot);
        title_desktop_addons_error("late worker error");
        assert(pending_addon_kind == 3 && strstr(addon_notice, "Close and reopen Add-ons"));
        assert(forever_menu_addon_pause(&menu, addon_notice));
        assert(menu.page == FOREVER_MENU_NOTICE);
        forever_menu_render(&menu, text, sizeof(text));
        assert(strstr(text, "Close and reopen Add-ons") && !strstr(text, "Type key"));
        forever_menu_move(&menu, 1);
        forever_menu_addon_snapshot(&menu, &pending_addon_snapshot);
        forever_menu_addon_error(&menu, "late worker error");
        assert(strstr(menu.status, "Close and reopen Add-ons"));
        assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_NONE);
        assert(forever_menu_back(&menu).command == FOREVER_MENU_COMMAND_CLOSE);
    }
    menu.page = FOREVER_MENU_MAIN;
    assert(!forever_menu_addon_pause(&menu, addon_notice) && menu.page == FOREVER_MENU_MAIN);
    menu.page = FOREVER_MENU_CLOSED;
    assert(!forever_menu_addon_pause(&menu, addon_notice) && menu.page == FOREVER_MENU_CLOSED);
}
int main(void)
{
    char *renderer[] = {"wine", "Battle.net.exe", "--type=renderer", NULL};
    char *utility[] = {"wine", "Battle.net.exe", "--type=utility", NULL};
    char *updater[] = {"wine", "Battle.net.exe", "--updatepid=42", NULL};
    char *core[] = {"wine", "C:\\Forever\\Addons\\addonforge-core.exe", "serve", NULL};
    char *other_core[] = {"wine", "C:\\Other\\addonforge-core.exe", NULL};
    char *renderer_extra[] = {"wine", "Battle.net.exe", "--type=renderer-extra", NULL};
    char *other_renderer[] = {"wine", "C:\\Other\\tool.exe", "--type=renderer", NULL};
    char *renderer_path[] = {"wine", "C:\\Program Files (x86)\\Battle.net\\BATTLE.NET.exe",
                             "--log-severity=warning", "--type=renderer", NULL};

    fixture(HELD, 1);
    graceful_eof = 500;
    assert(admit(renderer) == HELPER);
    assert(stop_calls == 1 && !kill_calls && clock_ms == 500);
    assert(!addon_open && strstr(addon_notice, "Close and reopen Add-ons"));
    assert(occupied_slots() == HELD && slots[HELPER].nonce == 1000);
    pause_notice_consumer();

    fixture(HELD, 1);
    reap_after_kill = 1;
    assert(admit(renderer) == HELPER);
    assert(stop_calls == 1 && kill_calls == 1 && clock_ms >= 2000 && clock_ms <= 2100);
    fixture(HELD, 1);
    graceful_eof = 10;
    assert(admit(renderer_path) == HELPER && stop_calls == 1 && !kill_calls);

    fixture(HELD, 1);
    assert(admit(renderer) == -1 && errno == EAGAIN);
    assert(stop_calls == 1 && kill_calls == 1 && clock_ms <= 4000);
    assert(slots[HELPER].occupied && slots[HELPER].nonce == addonforge_nonce); /* no reuse before EOF */

    fixture(HELD, 0);
    assert(admit(renderer) == -1 && errno == EAGAIN && !stop_calls && !kill_calls);
    assert(admit(updater) == HELD && occupied_slots() == PW_CHILDREN); /* updater/game reserve is unchanged */

    fixture(HELD, 1);
    addonforge_nonce++; /* an old helper identity must never evict a reused slot */
    assert(admit(renderer) == -1 && errno == EAGAIN && !stop_calls && !kill_calls);
    fixture(HELD, 1);
    assert(admit(utility) == -1 && errno == EAGAIN && !stop_calls && !kill_calls);
    assert(admit(renderer_extra) == -1 && errno == EAGAIN && !stop_calls && !kill_calls);
    assert(admit(other_renderer) == -1 && errno == EAGAIN && !stop_calls && !kill_calls);

    fixture(HELPER, 0);
    assert(admit(core) == HELPER && addonforge_nonce == 1000);
    fixture(HELD, 0);
    assert(admit(core) == -1 && errno == EAGAIN && occupied_slots() == HELD);
    assert(admit(other_core) == HELD && !addonforge_nonce); /* fixed managed path only */

    fixture(HELPER, 0);
    assert(admit(renderer) == HELPER && !stop_calls && !kill_calls && !clock_ms);
    fixture(HELD, 1);
    stop_during_wait = 1;
    assert(admit(renderer) == -1 && errno == EAGAIN && !kill_calls);

    /* The console refuses the child: the slot is free again, the log names the code. */
    struct wine_ps5_title_spawn refused = {.argv = updater, .windows_pid = 200};
    fixture(HELPER, 0);
    assert(admit(updater) == HELPER);
    slots[HELPER].handle = (int32_t)0x80aa0008;
    assert(refuse_local_process(HELPER, 1000, &refused) == EAGAIN);
    assert(!slots[HELPER].occupied && occupied_slots() == HELPER);
    assert(strstr(last_log, "reason=system-limit code=0x80aa0008") && strstr(last_log, "Battle.net.exe"));
    assert(admit(updater) == HELPER);
    slots[HELPER].handle = -1;
    assert(refuse_local_process(HELPER, 1000, &refused) == EIO && !slots[HELPER].occupied);
    assert(strstr(last_log, "reason=spawn-failed code=0xffffffff"));

    puts("supervisor admission: helper yield/forced EOF, stale identity, reserve, local-process refusal "
         "and shutdown PASS");
    return 0;
}
