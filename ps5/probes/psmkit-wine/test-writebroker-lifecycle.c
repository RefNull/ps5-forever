/* Run the complete write broker, main() included, as real processes on a
 * loopback port: only one broker starts, a broker that cannot own the port
 * never touches the running one's token, another program on the port is
 * reported, an older broker is still replaced, and startup repair leaves
 * owner-private entries alone. Only the console log is a host stub. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ps5/klog.h>
#include "wine/ps5_writebroker.h"

/* Every broker file is relative to the scenario's scratch folder. */
#undef WB_TOKEN_FILE
#define WB_TOKEN_FILE "wb-token"
#undef WB_PORT
#define WB_PORT test_port
#define WB_LOCK_FILE "wb-lock"
#define WB_REPAIR_DATA_ROOT "repair-data"
#define WB_STARTUP_REPAIR test_repair
#define WB_STARTUP_WRITE_TOKEN test_write_token

static int test_port, failures;
static pid_t children[8];
static unsigned child_count;

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void note(const char *file)
{
    int fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0 || write(fd, "x\n", 2) != 2) abort();
    close(fd);
}

static int count_lines(const char *file)
{
    char buffer[256];
    int fd = open(file, O_RDONLY), count = 0;
    ssize_t got;

    if (fd < 0) return 0;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
        for (ssize_t i = 0; i < got; i++) count += buffer[i] == '\n';
    close(fd);
    return count;
}

int klog_printf(const char *format, ...)
{
    char line[600];
    va_list args;
    int fd = open("broker.log", O_WRONLY | O_CREAT | O_APPEND, 0644), length;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (fd < 0) return -1;
    if (length > (int)sizeof(line) - 1) length = (int)sizeof(line) - 1;
    if (write(fd, line, (size_t)length) != length) length = -1;
    close(fd);
    return length;
}

static int repair_permissions(void);
static int write_token(void);

/* The production repair and token write, counted; "slow-repair" holds a
 * starting broker in its repair, bound but not yet listening. */
static int test_repair(void)
{
    note("repairs");
    if (!access("slow-repair", F_OK)) usleep(2000000);
    return repair_permissions();
}

static int test_write_token(void)
{
    note("token-writes");
    return write_token();
}

#define main writebroker_main
#include "writebroker-lifecycle.inc"
#undef main

static int free_port(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(fd, (struct sockaddr *)&address, &size)) abort();
    close(fd);
    return ntohs(address.sin_port);
}

static pid_t spawn(int (*entry)(void))
{
    pid_t pid = fork();

    if (pid < 0) abort();
    if (!pid)
    {
        /* a broker process inherits none of the test's sockets */
        for (int fd = 3; fd < 256; fd++) close(fd);
        _exit(entry());
    }
    children[child_count++] = pid;
    return pid;
}

/* exit status, or -1 while it still runs after ms */
static int wait_exit(pid_t pid, int ms)
{
    for (int waited = 0;; waited += 20)
    {
        int status;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
        {
            for (unsigned i = 0; i < child_count; i++)
                if (children[i] == pid) children[i] = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        }
        if (waited >= ms) return -1;
        usleep(20000);
    }
}

static void stop_children(void)
{
    for (unsigned i = 0; i < child_count; i++)
        if (children[i])
        {
            kill(children[i], SIGKILL);
            waitpid(children[i], NULL, 0);
        }
    child_count = 0;
}

static int read_token(unsigned char out[WB_TOKEN_BYTES])
{
    int fd = open(WB_TOKEN_FILE, O_RDONLY), ok;
    ok = fd >= 0 && read(fd, out, WB_TOKEN_BYTES) == WB_TOKEN_BYTES;
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}

static void put_token(const char *contents)
{
    int fd = open(WB_TOKEN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, contents, WB_TOKEN_BYTES) != WB_TOKEN_BYTES) abort();
    close(fd);
}

static int connect_port(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(test_port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct timeval timeout = {.tv_sec = 1};
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) abort();
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)))
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* the title's view: a broker of this version accepts the token file's token */
static int broker_answers(void)
{
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply = {0};
    int fd, ok;

    if (read_token(hello.token) || (fd = connect_port()) < 0) return 0;
    ok = !transfer(fd, &hello, sizeof(hello), 1) && !transfer(fd, &reply, sizeof(reply), 0) &&
         reply.magic == WB_MAGIC && !reply.result;
    close(fd);
    return ok;
}

static int wait_until(int (*condition)(void), int ms)
{
    for (int waited = 0; waited <= ms; waited += 20)
    {
        if (condition()) return 1;
        usleep(20000);
    }
    return 0;
}

static int repairing(void) { return count_lines("repairs") > 0; }
static int port_accepts(void)
{
    int fd = connect_port();
    if (fd >= 0) close(fd);
    return fd >= 0;
}

static int log_contains(const char *text)
{
    char buffer[16384];
    int fd = open("broker.log", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, buffer, sizeof(buffer) - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 0) return 0;
    buffer[got] = 0;
    return strstr(buffer, text) != NULL;
}

static void enter(const char *name)
{
    if (mkdir(name, 0755) || chdir(name) || mkdir("repair-data", 0755)) abort();
    test_port = free_port();
}

static void leave(void)
{
    stop_children();
    if (chdir("..")) abort();
}

static void running_broker_kept(void)
{
    unsigned char first[WB_TOKEN_BYTES], after[WB_TOKEN_BYTES];
    pid_t running, late;

    enter("running");
    running = spawn(writebroker_main);
    check(wait_until(broker_answers, 5000) && !read_token(first), "the first broker starts");
    late = spawn(writebroker_main);
    check(wait_exit(late, 4000) == 0,
          "a second broker exits quietly while one of its version answers");
    check(waitpid(running, NULL, WNOHANG) == 0 && broker_answers(),
          "the running broker is not asked to exit");
    check(!read_token(after) && !memcmp(first, after, sizeof(first)),
          "the second broker leaves the running broker's token alone");
    check(count_lines("repairs") == 1 && count_lines("token-writes") == 1,
          "the second broker neither repairs nor writes a token");
    leave();
}

static void starting_broker_single_flight(void)
{
    pid_t first, late;

    enter("starting");
    put_token("token-of-before!");
    note("slow-repair");
    first = spawn(writebroker_main);
    check(wait_until(repairing, 3000), "the first broker starts its repair");
    late = spawn(writebroker_main);
    /* Linux refuses a connect to a bound port at once; macOS holds it until
     * the port listens, so the late copy may see the first one answer. */
    check(wait_exit(late, 5000) >= 0, "a broker sent while another is starting exits");
    check(wait_until(broker_answers, 5000), "the starting broker then answers");
    check(count_lines("repairs") == 1, "only the starting broker repairs");
    check(count_lines("token-writes") == 1, "only the starting broker writes the token");
    check(waitpid(first, NULL, WNOHANG) == 0, "the starting broker keeps running");
    leave();
}

static void foreign_port_holder(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(test_port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    unsigned char after[WB_TOKEN_BYTES];
    int one = 1, silent;
    pid_t late;

    enter("foreign");
    address.sin_port = htons(test_port);
    put_token("token-of-runner!");
    /* another program: it accepts (the kernel completes connections) and never answers */
    silent = socket(AF_INET, SOCK_STREAM, 0);
    if (silent < 0 || setsockopt(silent, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(silent, (struct sockaddr *)&address, sizeof(address)) || listen(silent, 8)) abort();
    late = spawn(writebroker_main);
    check(wait_exit(late, 8000) > 0, "a broker gives up when another program holds the port");
    check(!read_token(after) && !memcmp(after, "token-of-runner!", sizeof(after)),
          "a broker that cannot own the port never replaces the token");
    check(count_lines("repairs") == 0 && count_lines("token-writes") == 0,
          "a broker that cannot own the port neither repairs nor writes a token");
    check(log_contains("held by another program"),
          "the broker reports that another program holds its port");
    close(silent);
    leave();
}

/* A version 1 broker: it closes a connection on any other version's hello,
 * and exits on QUIT after its own. */
static int old_broker(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(test_port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int one = 1, listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8))
        return 2;
    for (;;)
    {
        struct wb_hello hello;
        struct wb_reply reply = {.magic = WB_MAGIC};
        struct wb_write request;
        int client = accept(listener, NULL, NULL);

        if (client < 0) return 3;
        if (!transfer(client, &hello, sizeof(hello), 0) && hello.version == 1 &&
            !transfer(client, &reply, sizeof(reply), 1) &&
            !transfer(client, &request, sizeof(request), 0) && request.magic == WB_QUIT_MAGIC)
            return 0;
        close(client);
    }
}

static void older_broker_replaced(void)
{
    pid_t old, replacement;

    enter("older");
    put_token("token-of-v1-run!");
    old = spawn(old_broker);
    check(wait_until(port_accepts, 3000), "the older broker listens");
    replacement = spawn(writebroker_main);
    check(wait_exit(old, 5000) == 0, "an older broker is asked to exit by version step-down");
    check(wait_until(broker_answers, 5000), "the new broker takes the port");
    check(waitpid(replacement, NULL, WNOHANG) == 0, "the new broker keeps running");
    leave();
}

static void fixture(const char *path, mode_t mode, int directory)
{
    int fd;

    if (directory) { if (mkdir(path, 0755)) abort(); }
    else if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 || close(fd)) abort();
    if (chmod(path, mode)) abort();
}

static mode_t mode_of(const char *path)
{
    struct stat status;
    if (lstat(path, &status)) abort();
    return status.st_mode & 07777;
}

static void repair_scope(void)
{
    struct wb_permission_repair repair = {0};
    static const char *const directories[] = {
        "repair-data/forever", "repair-data/forever/prefix",
        "repair-data/forever/prefix/drive_c",
    };

    enter("repair");
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
        fixture(directories[i], 0755, 1);
    fixture("repair-data/forever/prefix/drive_c/Shared", 0755, 1);
    fixture("repair-data/forever/prefix/drive_c/Shared/data.bin", 0644, 0);
    fixture("repair-data/forever/prefix/drive_c/OtherPayload", 0711, 1);
    fixture("repair-data/forever/prefix/drive_c/OtherPayload/state", 0700, 1);
    fixture("repair-data/forever/prefix/drive_c/OtherPayload/state/journal", 0644, 0);
    fixture("repair-data/forever/prefix/private.txt", 0600, 0);
    repair_all(&repair);
    check(mode_of("repair-data/forever/prefix/drive_c/Shared") == 0777 &&
          mode_of("repair-data/forever/prefix/drive_c/Shared/data.bin") == 0666,
          "startup repair opens entries a umask left read-only for others");
    check(mode_of("repair-data/forever/prefix/drive_c/OtherPayload") == 0711 &&
          mode_of("repair-data/forever/prefix/drive_c/OtherPayload/state") == 0700 &&
          mode_of("repair-data/forever/prefix/drive_c/OtherPayload/state/journal") == 0644,
          "startup repair leaves an owner-private folder and its contents alone at any path");
    check(mode_of("repair-data/forever/prefix/private.txt") == 0600,
          "startup repair leaves an owner-private file alone");
    leave();
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    char path[1024];

    signal(SIGPIPE, SIG_IGN);
    alarm(120);
    snprintf(path, sizeof(path), "%s/writebroker-lifecycle-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(path) || chdir(path)) abort();
    umask(022);

    running_broker_kept();
    starting_broker_single_flight();
    foreign_port_holder();
    older_broker_replaced();
    repair_scope();
    if (failures) return 1;
    puts("write broker lifecycle: single-flight start, token after port, foreign port holder, "
         "older broker replaced, private entries kept PASS");
    return 0;
}
