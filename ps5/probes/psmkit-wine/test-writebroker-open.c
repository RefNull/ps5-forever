#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wine/ps5_writebroker.h"

#define WB_CHUNK (1u << 20)
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char write_root[PATH_MAX + 64];
static char host_name[PATH_MAX + 64 + WB_MAX_PATH];
static int failures;

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void say(const char *format, ...)
{
    (void)format;
}

/* The title's user data root is the scratch tree, however the broker opens
 * a name; every other name is unchanged. The strlen bound never fires, but
 * keeps gcc from flagging the broker's open("/") as "/" + length. */
static const char *host_path(const char *path)
{
    size_t length = wb_path_root(path);

    if (!length || length > strlen(path)) return path;
    snprintf(host_name, sizeof(host_name), "%s%s", write_root, path + length);
    return host_name;
}

#define WB_WRITE_ROOT write_root
#define open(path, ...) open(host_path(path), __VA_ARGS__)
#include "writebroker-open.inc"
#undef open

/* One write request through the extracted serve_write: its reply, or
 * INT32_MIN when it dropped the connection. */
static int32_t write_request(const char *path, const char *data)
{
    struct wb_write request = {.magic = WB_MAGIC, .path_length = (uint32_t)strlen(path),
                               .length = (uint32_t)strlen(data)};
    struct wb_reply reply = {0};
    unsigned char *buffer = malloc(WB_CHUNK);
    int pair[2];

    if (!buffer || socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) abort();
    if (transfer(pair[0], (void *)path, request.path_length, 1) ||
        transfer(pair[0], (void *)data, request.length, 1)) abort();
    if (serve_write(pair[1], &request, buffer)) reply.result = INT32_MIN;
    else if (transfer(pair[0], &reply, sizeof(reply), 0) || reply.magic != WB_MAGIC) abort();
    close(pair[0]);
    close(pair[1]);
    free(buffer);
    return reply.result;
}

static void make_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, contents, strlen(contents)) != (ssize_t)strlen(contents)) abort();
    close(fd);
}

static int file_is(const char *path, const char *contents)
{
    char actual[64] = {0};
    int fd = open(path, O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, actual, sizeof(actual) - 1);

    if (fd >= 0) close(fd);
    return got == (ssize_t)strlen(contents) && !memcmp(actual, contents, (size_t)got);
}

static void write_tests(const char *base)
{
    char root[PATH_MAX + 32], outside[PATH_MAX + 32], target[PATH_MAX + 32], name[PATH_MAX + 64];
    pid_t child;
    int status;

    snprintf(root, sizeof(root), "%s/root", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    snprintf(target, sizeof(target), "%s/outside/target", base);
    snprintf(write_root, sizeof(write_root), "%s/", root);
    if (mkdir(root, 0755) || mkdir(outside, 0755)) abort();
    make_file(target, "outside");
    snprintf(name, sizeof(name), "%s/file", root);
    make_file(name, "aaaa");
    snprintf(name, sizeof(name), "%s/sub", root);
    if (mkdir(name, 0755)) abort();
    snprintf(name, sizeof(name), "%s/sub/file", root);
    make_file(name, "cccc");
    snprintf(name, sizeof(name), "%s/leaf-link", root);
    if (symlink(target, name)) abort();
    snprintf(name, sizeof(name), "%s/directory-link", root);
    if (symlink(outside, name)) abort();
    snprintf(name, sizeof(name), "%s/fifo", root);
    if (mkfifo(name, 0644)) abort();

    check(write_request(WB_USER_ROOT "file", "bbbb") == 4, "regular file below the root is written");
    snprintf(name, sizeof(name), "%s/file", root);
    check(file_is(name, "bbbb"), "regular file holds the written bytes");
    check(write_request(WB_USER_ROOT "sub/file", "dddd") == 4, "nested regular file is written");
    snprintf(name, sizeof(name), "%s/sub/file", root);
    check(file_is(name, "dddd"), "nested regular file holds the written bytes");
    check(write_request(WB_USER_ROOT "missing", "xxxx") == -ENOENT, "missing file is not created");
    check(write_request(WB_USER_ROOT "sub", "xxxx") < 0, "directory is refused");
    check(write_request(WB_USER_ROOT "../outside/target", "xxxx") == -EACCES, "climbing out is refused");

    check(write_request(WB_USER_ROOT "leaf-link", "LEAF") < 0, "final symlink out of the root is refused");
    check(file_is(target, "outside"), "final symlink target is unchanged");
    check(write_request(WB_USER_ROOT "directory-link/target", "DIRS") < 0,
          "intermediate symlink out of the root is refused");
    check(file_is(target, "outside"), "intermediate symlink target is unchanged");

    /* No reader: a blocking open would wait forever, so run it where a
     * watchdog can end it. */
    fflush(NULL);
    if ((child = fork()) < 0) abort();
    if (!child)
    {
        alarm(5);
        _exit(write_request(WB_USER_ROOT "fifo", "FIFO") < 0 ? 0 : 1);
    }
    if (waitpid(child, &status, 0) != child) abort();
    check(!WIFSIGNALED(status), "FIFO below the root does not block the serving thread");
    check(WIFEXITED(status) && !WEXITSTATUS(status), "FIFO below the root is refused");
}

static void accept_tests(void)
{
    static const int fatal[] = {EBADF, EINVAL, ENOTSOCK, EOPNOTSUPP};
    static const int now[] = {EINTR, ECONNABORTED, EPROTO};
    static const int later[] = {EAGAIN, EWOULDBLOCK, ENOBUFS, ENOMEM, EMFILE, ENFILE};

    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        check(accept_error_action(fatal[i]) < 0, "accept error without a listener is fatal");
    for (size_t i = 0; i < sizeof(now) / sizeof(now[0]); i++)
        check(accept_error_action(now[i]) == 0, "accept error of one connection retries at once");
    for (size_t i = 0; i < sizeof(later) / sizeof(later[0]); i++)
        check(accept_error_action(later[i]) > 0, "accept error from exhaustion retries after a pause");
}

int main(void)
{
    char scratch[PATH_MAX], base[PATH_MAX], cleanup[PATH_MAX + 16];
    const char *tmp = getenv("TMPDIR");

    signal(SIGPIPE, SIG_IGN);
    snprintf(scratch, sizeof(scratch), "%s/writebroker-open-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    /* every component of the root is opened without following a link */
    if (!mkdtemp(scratch) || !realpath(scratch, base)) abort();
    write_tests(base);
    accept_tests();
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", base);
    if (system(cleanup)) failures++;
    if (failures) return 1;
    puts("write broker: no-follow, non-blocking write open and accept-error policy PASS");
    return 0;
}
