/* The in-process server's namespace client (Wine server/fd.c) against the
 * complete write broker running as a real process on a loopback port: once
 * a local call was slow, creates, mkdirs, renames and removals go to the
 * broker with Windows-relevant answers kept, refusals and a lost broker fall
 * back to the title's own call, and the broker stays inside the user data
 * root, follows no link, checks the entry's identity (inode number and type,
 * not the title's dev) and the title's access, and leaves its own files
 * alone; an identity refusal leaves the call to the title. Only the console log is a host stub. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/* The broker's files are relative to the scratch folder; the title's user
 * data root /data/forever/ is the scratch folder's root/. */
#undef WB_TOKEN_FILE
#define WB_TOKEN_FILE "wb-token"
#undef WB_PORT
#define WB_PORT test_port
#define WB_LOCK_FILE "wb-lock"
#define WB_REPAIR_DATA_ROOT "absent-repair-data"
#define WB_WRITE_ROOT write_root

static int test_port, failures;
static char write_root[PATH_MAX + 8];

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
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

#define main writebroker_main
#include "writebroker-namespace.inc"
#undef main

/* The in-process server's surroundings: its trace, and the title's own
 * calls, which here reach the scratch root and are counted. */
static int inprocess_server = 1;
static char last_trace[256];
static int local_creates, local_opens, local_mkdirs, local_unlinks, local_renames;

static void inprocess_trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(last_trace, sizeof(last_trace), format, args);
    va_end(args);
}

static const char *host_path(const char *path, int *counter)
{
    static char names[2][PATH_MAX + WB_MAX_PATH];
    static int next;
    size_t length = wb_path_root(path);
    char *name;

    if (!length) return path;
    if (counter) ++*counter;
    name = names[next++ % 2];
    snprintf(name, sizeof(names[0]), "%s%s", write_root, path + length);
    return name;
}

static const char *open_path(const char *path, int flags, ...)
{
    if (wb_path_root(path) && (flags & O_CREAT)) local_creates++;
    return host_path(path, &local_opens);
}

#define open(path, ...) open(open_path(path, __VA_ARGS__), __VA_ARGS__)
#define mkdir(path, mode) mkdir(host_path(path, &local_mkdirs), mode)
#define unlink(path) unlink(host_path(path, &local_unlinks))
#define rmdir(path) rmdir(host_path(path, &local_unlinks))
#define rename(from, to) rename(host_path(from, &local_renames), host_path(to, NULL))
#include "wine-namespace.inc"
#undef open
#undef mkdir
#undef unlink
#undef rmdir
#undef rename

static void reset_counts(void)
{
    local_creates = local_opens = local_mkdirs = local_unlinks = local_renames = 0;
}

/* the title's name, and the same entry's name in the scratch tree */
static const char *T(const char *relative)
{
    static char names[4][WB_MAX_PATH];
    static int next;
    char *name = names[next++ % 4];
    snprintf(name, sizeof(names[0]), WB_USER_ROOT "%s", relative);
    return name;
}

static const char *H(const char *relative)
{
    static char names[4][PATH_MAX + WB_MAX_PATH];
    static int next;
    char *name = names[next++ % 4];
    snprintf(name, sizeof(names[0]), "%s%s", write_root, relative);
    return name;
}

static void put_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
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

static int exists(const char *path)
{
    struct stat st;
    return !lstat(path, &st);
}

static struct stat stat_of(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) abort();
    return st;
}

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

static int connect_broker(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(test_port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply = {0};
    struct timeval timeout = {.tv_sec = 5};
    int fd, token = open(WB_TOKEN_FILE, O_RDONLY), ok;

    ok = token >= 0 && read(token, hello.token, sizeof(hello.token)) == (ssize_t)sizeof(hello.token);
    if (token >= 0) close(token);
    if (!ok || (fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        transfer(fd, &hello, sizeof(hello), 1) || transfer(fd, &reply, sizeof(reply), 0) ||
        reply.magic != WB_MAGIC || reply.result)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* One namespace request as any token holder may send it: the broker's
 * answer, or INT32_MIN when it dropped the connection. */
static int32_t raw(uint32_t magic, const char *path, const char *target, struct wb_namespace ns)
{
    struct wb_write request = {.magic = magic, .path_length = (uint32_t)strlen(path),
                               .length = target ? (uint32_t)strlen(target) : 0};
    struct wb_reply reply = {0};
    int fd = connect_broker();

    if (fd < 0) abort();
    if (transfer(fd, &request, sizeof(request), 1) || transfer(fd, &ns, sizeof(ns), 1) ||
        transfer(fd, (void *)path, request.path_length, 1) ||
        (target && transfer(fd, (void *)target, request.length, 1)) ||
        transfer(fd, &reply, sizeof(reply), 0) || reply.magic != WB_MAGIC)
        reply.result = INT32_MIN;
    close(fd);
    return reply.result;
}

static struct wb_namespace as_title(uint32_t flags, uint32_t mode, const char *entry)
{
    struct wb_namespace ns = {.flags = flags, .mode = mode, .uid = geteuid(), .gid = getegid()};
    if (entry)
    {
        struct stat st = stat_of(entry);
        ns.mode |= st.st_mode & S_IFMT;
        ns.dev = (uint64_t)st.st_dev;
        ns.ino = (uint64_t)st.st_ino;
    }
    return ns;
}

static int log_has(const char *text)
{
    char contents[1 << 16] = {0};
    int fd = open("broker.log", O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, contents, sizeof(contents) - 1);

    if (fd >= 0) close(fd);
    return got > 0 && strstr(contents, text);
}

/* The broker does not find the entry the title named (on the console, the
 * title's mount differed): ESTALE is no answer about the entry, so the
 * title's own call decides and the entry really goes, or the title's own
 * error stands. fd is open on root/other. */
static void identity_fallback_tests(int fd)
{
    char refused[64];
    struct stat st;
    int decoy, ret;

    snprintf(refused, sizeof(refused), "refused %d ", -ESTALE);
    reset_counts();
    put_file(H("victim"), "v");
    st = stat_of(H("victim"));
    st.st_ino ^= 0x5a5a;
    errno = 0;
    ret = ns_unlink(T("victim"), &st);
    check(!ret && local_unlinks == 1 && !exists(H("victim")),
          "an identity refusal leaves the unlink to the title, and the file is gone");
    check(strstr(last_trace, refused) != NULL, "the title traces the identity refusal");

    if (mkdir(H("vdir"), 0777)) abort();
    st = stat_of(H("vdir"));
    st.st_ino ^= 0x5a5a;
    check(!ns_unlink(T("vdir"), &st) && local_unlinks == 2 && !exists(H("vdir")),
          "an identity refusal leaves the rmdir to the title, and the directory is gone");

    if (mkdir(H("vfull"), 0777)) abort();
    put_file(H("vfull/x"), "x");
    st = stat_of(H("vfull"));
    st.st_ino ^= 0x5a5a;
    errno = 0;
    check(ns_unlink(T("vfull"), &st) == -1 && errno == ENOTEMPTY && local_unlinks == 3 &&
          exists(H("vfull/x")), "after an identity refusal the title's own error stands, not ENOENT");

    put_file(H("decoy"), "d");
    if ((decoy = open(H("decoy"), O_RDONLY)) < 0) abort();
    errno = 0;
    ret = ns_rename(T("other"), T("moved-local"), 0, decoy);
    check(!ret && local_renames == 1 && !exists(H("other")) && file_is(H("moved-local"), "x"),
          "an identity refusal leaves the rename to the title, and the file is renamed");
    close(decoy);
    check(!ns_rename(T("moved-local"), T("other"), 0, fd) && local_renames == 1 && file_is(H("other"), "x"),
          "a matching identity is renamed by the broker");
}

/* The title's dev comes from another mount; the inode number and file type
 * identify the entry, and the broker logs both sides of a mismatch. */
static void identity_tests(void)
{
    char line[160];
    struct wb_namespace ns;
    struct stat st;

    put_file(H("devfile"), "d");
    ns = as_title(0, 0, H("devfile"));
    ns.dev ^= 0x5a5a00;
    check(raw(WB_RENAME_MAGIC, T("devfile"), T("devfile2"), ns) == 0 && file_is(H("devfile2"), "d"),
          "a rename with another dev but the same inode is accepted");
    check(raw(WB_UNLINK_MAGIC, T("devfile2"), NULL, ns) == 0 && !exists(H("devfile2")),
          "an unlink with another dev but the same inode is accepted");

    put_file(H("typed"), "t");
    st = stat_of(H("typed"));
    ns = as_title(0, 0, H("typed"));
    ns.mode = S_IFDIR;
    check(raw(WB_UNLINK_MAGIC, T("typed"), NULL, ns) == -ESTALE && exists(H("typed")),
          "the same inode with another file type is refused");
    ns = as_title(0, 0, H("typed"));
    ns.ino ^= 0x5a5a;
    ns.dev = 0xd00df00dull;
    check(raw(WB_UNLINK_MAGIC, T("typed"), NULL, ns) == -ESTALE && exists(H("typed")),
          "another inode is refused");
    snprintf(line, sizeof(line), "title dev=d00df00d ino=%llx type=%o; broker dev=%llx ino=%llx type=%o",
             (unsigned long long)ns.ino, (unsigned)S_IFREG, (unsigned long long)st.st_dev,
             (unsigned long long)st.st_ino, (unsigned)S_IFREG);
    check(log_has(line), "a refusal logs the title's and the broker's dev and inode in hex");
}

static void routing_tests(void)
{
    struct stat st, other;
    int fd, ret;

    reset_counts();
    fd = ns_open(T("before"), O_CREAT | O_RDWR, 0644);
    check(fd >= 0 && local_creates == 1, "before any slow call a create stays local");
    if (fd >= 0) close(fd);
    ns_broker_note_local("create", ns_broker_now_us());
    check(!ns_broker_throttled, "a fast local call does not route to the broker");
    ns_broker_note_local("create", ns_broker_now_us() - 250000);
    check(ns_broker_throttled && strstr(last_trace, "reason=local-slow op=create"),
          "a 250 ms local call routes to the broker and says so");

    reset_counts();
    fd = ns_open(T("made"), O_CREAT | O_EXCL | O_RDWR, 0640);
    check(fd >= 0 && !local_creates && local_opens == 1, "broker creates, title only opens");
    st = stat_of(H("made"));
    check(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0640, "created with the mode asked, not the umask");
    check(st.st_uid == geteuid(), "created file is owned by the title");
    if (fd < 0 || write(fd, "x", 1) != 1) abort();
    close(fd);

    reset_counts();
    errno = 0;
    check(ns_open(T("made"), O_CREAT | O_EXCL | O_RDWR, 0644) == -1 && errno == EEXIST && !local_creates,
          "exclusive create of an existing file is EEXIST from the broker");
    fd = ns_open(T("made"), O_CREAT | O_RDWR, 0644);
    check(fd >= 0 && !local_creates && file_is(H("made"), "x"), "open-if of an existing file opens it");
    errno = 0;
    check(ns_open(T("no-dir/file"), O_CREAT | O_RDWR, 0644) == -1 && errno == ENOENT && !local_creates,
          "create below a missing directory is ENOENT from the broker");

    check(!ns_mkdir(T("dir"), 0750) && !local_mkdirs, "broker makes a directory");
    st = stat_of(H("dir"));
    check(S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0750 && st.st_uid == geteuid(),
          "directory has the mode asked and the title as owner");
    errno = 0;
    check(ns_mkdir(T("dir"), 0750) == -1 && errno == EEXIST && !local_mkdirs, "existing directory is EEXIST");

    check(!ns_rename(T("made"), T("dir/moved"), 0, fd) && !local_renames, "broker renames the open file");
    check(!exists(H("made")) && file_is(H("dir/moved"), "x"), "renamed file is at its new name");
    put_file(H("other"), "o");
    errno = 0;
    check(ns_rename(T("dir/moved"), T("other"), 0, fd) == -1 && errno == EEXIST && !local_renames,
          "rename without replace onto an existing name is EEXIST");
    check(file_is(H("other"), "o") && exists(H("dir/moved")), "refused rename changes nothing");
    check(!ns_rename(T("dir/moved"), T("other"), 1, fd) && !local_renames && file_is(H("other"), "x"),
          "rename with replace replaces a regular file");

    if (mkdir(H("adir"), 0755)) abort();
    ret = ns_rename(T("other"), T("adir"), 1, fd);
    check(ret == -1 && local_renames == 1, "broker refuses replacing a directory; the title's own call decides");
    check(exists(H("other")) && S_ISDIR(stat_of(H("adir")).st_mode), "directory target survives");

    if (fstat(fd, &other)) abort();
    identity_fallback_tests(fd);
    close(fd);
    reset_counts();
    check(!ns_unlink(T("other"), &other) && !exists(H("other")) && !local_unlinks, "broker removes the file");
    st = stat_of(H("dir"));
    check(!ns_unlink(T("dir"), &st) && !exists(H("dir")), "broker removes an empty directory");
    if (mkdir(H("full"), 0777)) abort();
    put_file(H("full/x"), "x");
    st = stat_of(H("full"));
    errno = 0;
    check(ns_unlink(T("full"), &st) == -1 && errno == ENOTEMPTY && !local_unlinks,
          "a non-empty directory is ENOTEMPTY");
    check(ns_broker_fd >= 0, "one connection serves the requests");
}

static void confinement_tests(const char *outside)
{
    char outside_target[PATH_MAX + 16], outside_new[PATH_MAX + 16], link_target[PATH_MAX + 16];
    struct wb_namespace ns;
    uid_t stranger = geteuid() + 1000;

    snprintf(outside_target, sizeof(outside_target), "%s/target", outside);
    put_file(outside_target, "outside");
    if (symlink(outside, H("dirlink")) || symlink(outside_target, H("leaflink"))) abort();
    put_file(H("plain"), "p");
    put_file(H(".writebroker-token"), "t");

    check(raw(WB_CREATE_MAGIC, T("dirlink/new"), NULL, as_title(0, 0644, NULL)) != 0,
          "create through a directory link is refused");
    snprintf(outside_new, sizeof(outside_new), "%s/new", outside);
    check(!exists(outside_new), "nothing is created outside the root");
    check(raw(WB_MKDIR_MAGIC, T("dirlink/newdir"), NULL, as_title(0, 0755, NULL)) != 0,
          "mkdir through a directory link is refused");
    check(raw(WB_CREATE_MAGIC, T("leaflink"), NULL, as_title(WB_NS_EXCL, 0644, NULL)) == -EEXIST,
          "exclusive create on a final link is EEXIST");
    check(raw(WB_CREATE_MAGIC, T("leaflink"), NULL, as_title(0, 0644, NULL)) == 0 &&
          file_is(outside_target, "outside") && readlink(H("leaflink"), link_target, sizeof(link_target)) > 0,
          "create on a final link leaves the link and its target alone");
    check(raw(WB_UNLINK_MAGIC, T("dirlink/target"), NULL, as_title(0, 0, outside_target)) != 0 &&
          exists(outside_target), "unlink through a directory link is refused");
    check(raw(WB_UNLINK_MAGIC, T("leaflink"), NULL, as_title(0, 0, outside_target)) == -ESTALE &&
          exists(outside_target) && exists(H("leaflink")), "a final link is not its target");
    check(raw(WB_RENAME_MAGIC, T("plain"), T("dirlink/stolen"), as_title(0, 0, H("plain"))) != 0 &&
          exists(H("plain")), "rename into a directory link is refused");
    check(raw(WB_RENAME_MAGIC, T("plain"), WB_USER_ROOT "../stolen", as_title(0, 0, H("plain"))) == -EACCES &&
          exists(H("plain")), "rename target climbing out is refused");
    check(raw(WB_UNLINK_MAGIC, WB_USER_ROOT "../outside/target", NULL, as_title(0, 0, outside_target)) == -EACCES &&
          exists(outside_target), "unlink climbing out is refused");
    check(raw(WB_UNLINK_MAGIC, T(".writebroker-token"), NULL, as_title(0, 0, H(".writebroker-token"))) == -EACCES &&
          exists(H(".writebroker-token")), "the broker's own token is not the title's to remove");
    check(raw(WB_RENAME_MAGIC, T("plain"), T(".writebroker-lock"), as_title(0, 0, H("plain"))) == -EACCES &&
          exists(H("plain")), "nothing is renamed onto the broker's own files");

    if (mkdir(H("locked"), 0755)) abort();
    ns = as_title(0, 0644, NULL);
    ns.uid = stranger;
    ns.gid = getegid() + 1000;
    check(raw(WB_CREATE_MAGIC, T("locked/f"), NULL, ns) == -EACCES && !exists(H("locked/f")),
          "a directory the title may not write takes no new entry");
    check(raw(WB_CREATE_MAGIC, T("locked/f"), NULL, as_title(0, 0644, NULL)) == 0 && exists(H("locked/f")),
          "the directory's owner may create in it");
    if (mkdir(H("sticky"), 0777) || chmod(H("sticky"), 01777)) abort();
    put_file(H("sticky/mine"), "m");
    ns = as_title(0, 0, H("sticky/mine"));
    ns.uid = stranger;
    check(raw(WB_UNLINK_MAGIC, T("sticky/mine"), NULL, ns) == -EACCES && exists(H("sticky/mine")),
          "a sticky directory keeps another owner's entry");

    if (mkdir(H("open"), 0777) || chmod(H("open"), 0777)) abort();
    ns = as_title(0, 0644, NULL);
    ns.uid = stranger;
    if (geteuid())
        check(raw(WB_CREATE_MAGIC, T("open/foreign"), NULL, ns) == -EPERM && !exists(H("open/foreign")),
              "a file the title could not own is removed again");
    else
        check(raw(WB_CREATE_MAGIC, T("open/foreign"), NULL, ns) == 0 &&
              stat_of(H("open/foreign")).st_uid == stranger, "a new file is owned by the title's uid");

    check(raw(WB_CREATE_MAGIC, T("flags"), NULL, as_title(0x80, 0644, NULL)) == -EINVAL && !exists(H("flags")),
          "unknown flags are refused");
    check(raw(WB_UNLINK_MAGIC, T("plain"), NULL, as_title(0, 0, NULL)) == -EINVAL && exists(H("plain")),
          "unlink without the entry's identity is refused");
}

static void fallback_test(pid_t broker)
{
    int fd, status;

    kill(broker, SIGKILL);
    waitpid(broker, &status, 0);
    reset_counts();
    fd = ns_open(T("after-broker"), O_CREAT | O_RDWR, 0644);
    check(fd >= 0 && local_creates == 1 && ns_broker_fd == -1, "a lost broker leaves the create to the title");
    if (fd >= 0) close(fd);
    fd = ns_open(T("after-broker-2"), O_CREAT | O_RDWR, 0644);
    check(fd >= 0 && local_creates == 2 && strstr(last_trace, "unavailable"),
          "an absent broker leaves the create to the title");
    if (fd >= 0) close(fd);
}

int main(void)
{
    char scratch[PATH_MAX], base[PATH_MAX], outside[PATH_MAX + 16], cleanup[2 * PATH_MAX + 64];
    const char *tmp = getenv("TMPDIR");
    pid_t broker;
    int fd = -1;

    signal(SIGPIPE, SIG_IGN);
    snprintf(scratch, sizeof(scratch), "%s/writebroker-namespace-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(scratch) || !realpath(scratch, base) || chdir(base)) abort();
    snprintf(write_root, sizeof(write_root), "%s/root/", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    if (mkdir("root", 0777) || chmod("root", 0777) || mkdir(outside, 0755)) abort();
    /* the broker sets modes itself, whatever its umask */
    umask(077);
    test_port = free_port();
    fflush(NULL);
    if ((broker = fork()) < 0) abort();
    if (!broker)
    {
        for (int i = 3; i < 256; i++) close(i);
        _exit(writebroker_main());
    }
    for (int waited = 0; waited < 10000 && (fd = connect_broker()) < 0; waited += 50) usleep(50000);
    if (fd < 0)
    {
        fprintf(stderr, "FAIL broker did not start\n");
        kill(broker, SIGKILL);
        return 1;
    }
    close(fd);

    routing_tests();
    identity_tests();
    confinement_tests(outside);
    fallback_test(broker);

    snprintf(cleanup, sizeof(cleanup), "chmod -R u+w '%s'; rm -rf '%s'", base, base);
    if (system(cleanup)) failures++;
    if (failures) return 1;
    puts("write broker: namespace requests from the in-process server PASS");
    return 0;
}
