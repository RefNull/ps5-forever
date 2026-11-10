/* writebroker: performs a PS5 title's file writes, and once it is throttled
 * its creates, renames and removals, from a payload process, which the
 * console's throttling does not limit (see ps5-wine
 * include/wine/ps5_writebroker.h for why and for the protocol).
 *
 * The title sends it through the ELF loader at start when no broker of its
 * protocol version (WB_VERSION) answers, and its watch and permission-repair
 * restart may send more copies. Only one runs: a copy first takes
 * WB_LOCK_FILE and then the port, and exits when a broker of its version
 * already answers or is starting. Once it owns the port it repairs
 * permissions, writes a fresh token to WB_TOKEN_FILE, listens on
 * 127.0.0.1:WB_PORT, and serves each connection on its own thread. An older
 * version holding the port is asked to exit and replaced. Logs go to the
 * console log with the prefix "writebroker: ". */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <sys/time.h>
#include <unistd.h>
#include <ps5/klog.h>
#include "wine/ps5_writebroker.h"
#include "../probes/psmkit-wine/writebroker-menu.h"

#ifndef WB_MENU_PREFIX_ROOT
#define WB_MENU_PREFIX_ROOT WB_USER_ROOT "prefix"
#endif
#define WB_MENU_REMOVE_DEPTH 64u
#ifndef WB_WRITE_ROOT
#define WB_WRITE_ROOT WB_USER_ROOT
#endif

#define WB_CHUNK (1u << 20)

#ifndef WB_REPAIR_DATA_ROOT
#define WB_REPAIR_DATA_ROOT "/data"
#endif
#ifndef WB_REPAIR_MAX_ENTRIES
#define WB_REPAIR_MAX_ENTRIES 100000u
#endif
#ifndef WB_REPAIR_MAX_DEPTH
#define WB_REPAIR_MAX_DEPTH 64u
#endif
#ifndef WB_REPAIR_FSTATAT
#define WB_REPAIR_FSTATAT fstatat
#endif
#ifndef WB_REPAIR_READDIR
#define WB_REPAIR_READDIR readdir
#endif
#ifndef WB_REPAIR_CLOSEDIR
#define WB_REPAIR_CLOSEDIR closedir
#endif
#ifndef WB_LOCK_FILE
#define WB_LOCK_FILE WB_USER_ROOT ".writebroker-lock"
#endif

static unsigned char token[WB_TOKEN_BYTES];
static const char *token_file = WB_TOKEN_FILE;
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    klog_printf("writebroker: %s\n", line);
}

/* all of size bytes, or -1 with errno (EPIPE at a closed connection) */
static int transfer(int fd, void *buffer, size_t size, int sending)
{
    unsigned char *bytes = buffer;
    size_t done = 0;

    while (done < size)
    {
        ssize_t count = sending ? send(fd, bytes + done, size - done, MSG_NOSIGNAL)
                                : recv(fd, bytes + done, size - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EPIPE; return -1; }
        done += (size_t)count;
    }
    return 0;
}

/* receive and drop length bytes, keeping the stream in step after a refusal */
static int discard(int client, unsigned char *buffer, uint32_t length)
{
    while (length)
    {
        uint32_t chunk = length < WB_CHUNK ? length : WB_CHUNK;
        if (transfer(client, buffer, chunk, 0)) return -1;
        length -= chunk;
    }
    return 0;
}

static int menu_remove_index(const char *path)
{
    for (unsigned i = 0; wb_menu_remove_relative(i, 1); i++)
    {
        char allowed[WB_MAX_PATH + 1];
        int length = snprintf(allowed, sizeof(allowed), WB_USER_ROOT "prefix%s",
                              wb_menu_remove_relative(i, 1));
        if (length > 0 && length < (int)sizeof(allowed) && !strcmp(path, allowed)) return (int)i;
    }
    return -1;
}

static int menu_remove_path_allowed(const char *path)
{
    return menu_remove_index(path) >= 0;
}

static int menu_open_child_directory(int parent, const char *name)
{
    int child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return child < 0 ? -errno : child;
}

/* Open every component of root from the filesystem root, never following a
 * link. The returned descriptor anchors all later relative traversal. */
static int menu_open_prefix_root(const char *root)
{
    char component[WB_MAX_PATH + 1];
    const char *at = root;
    int current, next;

    if (!at || *at != '/') return -EINVAL;
    if ((current = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) return -errno;
    while (*at)
    {
        const char *end;
        size_t length;
        while (*at == '/') at++;
        if (!*at) break;
        end = strchr(at, '/');
        length = end ? (size_t)(end - at) : strlen(at);
        if (!length || length > WB_MAX_PATH ||
            (length == 1 && at[0] == '.') ||
            (length == 2 && at[0] == '.' && at[1] == '.'))
        {
            close(current);
            return -EINVAL;
        }
        memcpy(component, at, length);
        component[length] = 0;
        next = menu_open_child_directory(current, component);
        close(current);
        if (next < 0) return next;
        current = next;
        at += length;
    }
    return current;
}

/* Open every fixed relative ancestor, leaving the last component in leaf. */
static int menu_open_remove_parent(int root, const char *relative, char leaf[WB_MAX_PATH + 1])
{
    char path[WB_MAX_PATH + 1], *at, *slash;
    int current = root, next;
    size_t length = strlen(relative);

    if (!length || length > WB_MAX_PATH || relative[0] != '/') { close(root); return -EINVAL; }
    memcpy(path, relative + 1, length); /* includes the terminating NUL */
    at = path;
    while ((slash = strchr(at, '/')))
    {
        *slash = 0;
        if (!*at || !strcmp(at, ".") || !strcmp(at, "..")) { close(current); return -EINVAL; }
        next = menu_open_child_directory(current, at);
        close(current);
        if (next < 0) return next;
        current = next;
        at = slash + 1;
    }
    if (!*at || !strcmp(at, ".") || !strcmp(at, "..")) { close(current); return -EINVAL; }
    strcpy(leaf, at);
    return current;
}

static int menu_remove_at(int parent, const char *name, unsigned depth)
{
    struct stat before, opened;
    DIR *directory;
    struct dirent *entry;
    int child = -1, scan = -1, result = 0;

    if (depth > WB_MENU_REMOVE_DEPTH) return -ELOOP;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(before.st_mode)) return unlinkat(parent, name, 0) ? -errno : 0;
    child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) return -errno;
    if (fstat(child, &opened))
    {
        result = -errno;
        goto done;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
    {
        result = -ESTALE;
        goto done;
    }
    if ((scan = dup(child)) < 0) { result = -errno; goto done; }
    if (!(directory = fdopendir(scan))) { result = -errno; goto done; }
    scan = -1; /* owned by directory */
    errno = 0;
    while (!result && (entry = readdir(directory)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strchr(entry->d_name, '/')) { result = -EINVAL; break; }
        result = menu_remove_at(child, entry->d_name, depth + 1);
        errno = 0;
    }
    if (!result && errno) result = -errno;
    if (closedir(directory) && !result) result = -errno;
done:
    if (scan >= 0) close(scan);
    if (child >= 0) close(child);
    if (!result && unlinkat(parent, name, AT_REMOVEDIR)) result = -errno;
    return result;
}

static int menu_remove_tree(const char *path)
{
    char leaf[WB_MAX_PATH + 1];
    int index = menu_remove_index(path), root, parent, result;
    const char *relative;

    if (index < 0) return -EACCES;
    relative = wb_menu_remove_relative((unsigned)index, 1);
    if ((root = menu_open_prefix_root(WB_MENU_PREFIX_ROOT)) < 0) return root == -ENOENT ? 0 : root;
    parent = menu_open_remove_parent(root, relative, leaf);
    if (parent < 0) return parent == -ENOENT ? 0 : parent;
    result = menu_remove_at(parent, leaf, 0);
    close(parent);
    return result;
}

static int serve_menu_remove(int client, const struct wb_write *request)
{
    char path[WB_MAX_PATH + 1];
    struct wb_reply reply = {.magic = WB_MAGIC};

    if (!request->path_length || request->path_length > WB_MAX_PATH ||
        request->offset || request->length || request->reserved) return -1;
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;
    reply.result = menu_remove_path_allowed(path) ? menu_remove_tree(path) : -EACCES;
    if (reply.result) say("menu removal refused/failed %d: %s", reply.result, path);
    else say("menu removed: %s", path);
    return transfer(client, &reply, sizeof(reply), 1);
}

struct wb_permission_repair
{
    unsigned scanned;
    unsigned changed;
    unsigned skipped;
    unsigned failed;
    int result;
    dev_t device;
};

static int repair_entry_at(
    int parent, const char *name, unsigned depth, struct wb_permission_repair *repair);
static int repair_scan_directory(
    int directory_fd, unsigned depth, struct wb_permission_repair *repair);
static int write_token(void);

static int repair_open_data_root(dev_t *device)
{
    struct stat status;
    int fd = open(WB_REPAIR_DATA_ROOT,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0) return -errno;
    if (fstat(fd, &status))
    {
        int error = -errno;
        close(fd);
        return error;
    }
    if (!S_ISDIR(status.st_mode))
    {
        close(fd);
        return -ENOTDIR;
    }
    *device = status.st_dev;
    return fd;
}


/* Resolve a fixed path relative to the already-open /data descriptor. Every
 * component is a real directory on /data's own filesystem. */
static int repair_open_relative_directory(int data_fd, const char *relative, dev_t device)
{
    char component[WB_MAX_PATH + 1];
    const char *at = relative;
    int current, next;

    if (!at || !*at || *at == '/') return -EINVAL;
    if ((current = dup(data_fd)) < 0) return -errno;
    while (*at)
    {
        const char *end;
        struct stat status;
        size_t length;

        while (*at == '/') at++;
        if (!*at) break;
        end = strchr(at, '/');
        length = end ? (size_t)(end - at) : strlen(at);
        if (!length || length > WB_MAX_PATH ||
            (length == 1 && at[0] == '.') ||
            (length == 2 && at[0] == '.' && at[1] == '.'))
        {
            close(current);
            return -EINVAL;
        }
        memcpy(component, at, length);
        component[length] = 0;
        next = openat(current, component,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            int error = -errno;
            close(current);
            return error;
        }
        close(current);
        current = next;
        if (fstat(current, &status))
        {
            int error = -errno;
            close(current);
            return error;
        }
        if (!S_ISDIR(status.st_mode) || status.st_dev != device)
        {
            close(current);
            return status.st_dev == device ? -ENOTDIR : -EXDEV;
        }
        at += length;
    }
    return current;
}

static int repair_apply_mode(
    int fd, const struct stat *status, struct wb_permission_repair *repair)
{
    mode_t wanted = S_ISDIR(status->st_mode) || (status->st_mode & 0111) ? 0777 : 0666;

    if ((status->st_mode & 07777) == wanted) return 0;
    if (!fchmod(fd, wanted))
    {
        repair->changed++;
        return 0;
    }
    repair->failed++;
    if (!repair->result) repair->result = -errno;
    return -errno;
}

static int repair_entry_at(
    int parent, const char *name, unsigned depth, struct wb_permission_repair *repair)
{
    struct stat before, opened;
    mode_t wanted;
    int fd, flags, result = 0;

    if (repair->scanned >= WB_REPAIR_MAX_ENTRIES)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    repair->scanned++;
    if (depth > WB_REPAIR_MAX_DEPTH)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    if (WB_REPAIR_FSTATAT(parent, name, &before, AT_SYMLINK_NOFOLLOW))
    {
        result = -errno;
        goto failed;
    }
    if (before.st_dev != repair->device)
    {
        repair->skipped++;
        return 0;
    }
    if (S_ISLNK(before.st_mode) ||
        (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)))
    {
        repair->skipped++;
        return 0;
    }
    if (S_ISREG(before.st_mode) && before.st_nlink > 1)
    {
        repair->skipped++;
        return 0;
    }
    /* The repair exists for content a root tool (FTP, another payload) wrote
     * with its default umask: others may read it but the title, which is not
     * its owner, can neither write nor chmod it, and Wine's PS5 security
     * mapping grants Battle.net's expected access only to world-rwx entries.
     * An entry others cannot read was made private by its owner on purpose
     * (the Add-ons worker's staging folder is one): leave it and what it holds. */
    if (!(before.st_mode & S_IROTH))
    {
        repair->skipped++;
        return 0;
    }
    wanted = S_ISDIR(before.st_mode) || (before.st_mode & 0111) ? 0777 : 0666;
    if (S_ISREG(before.st_mode) && (before.st_mode & 07777) == wanted)
        return 0;

    flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    if (S_ISDIR(before.st_mode)) flags |= O_DIRECTORY;
    if ((fd = openat(parent, name, flags)) < 0)
    {
        result = -errno;
        goto failed;
    }
    if (fstat(fd, &opened))
    {
        result = -errno;
        close(fd);
        goto failed;
    }
    if (opened.st_dev != repair->device)
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        (opened.st_mode & S_IFMT) != (before.st_mode & S_IFMT))
    {
        result = -ESTALE;
        close(fd);
        goto failed;
    }
    if ((S_ISREG(opened.st_mode) && opened.st_nlink > 1) || !(opened.st_mode & S_IROTH))
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    result = repair_apply_mode(fd, &opened, repair);
    if (S_ISDIR(opened.st_mode))
    {
        int scan_result = repair_scan_directory(fd, depth, repair);
        if (!result) result = scan_result;
    }
    close(fd);
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_scan_directory(
    int directory_fd, unsigned depth, struct wb_permission_repair *repair)
{
    DIR *directory;
    struct dirent *entry;
    int scan, result = 0;

    if ((scan = dup(directory_fd)) < 0)
    {
        result = -errno;
        goto failed;
    }
    if (!(directory = fdopendir(scan)))
    {
        result = -errno;
        close(scan);
        goto failed;
    }
    for (;;)
    {
        int child, read_error;
        errno = 0;
        entry = WB_REPAIR_READDIR(directory);
        read_error = errno;
        if (!entry)
        {
            if (read_error) result = -read_error;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strchr(entry->d_name, '/'))
        {
            result = -EINVAL;
            break;
        }
        child = repair_entry_at(directory_fd, entry->d_name, depth + 1, repair);
        if (child == -E2BIG)
        {
            result = child;
            break;
        }
    }
    if (WB_REPAIR_CLOSEDIR(directory) && !result) result = -errno;
    if (result && result != -E2BIG)
    {
        repair->failed++;
        if (!repair->result) repair->result = result;
    }
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_root(
    int data_fd, const char *relative, int recursive,
    struct wb_permission_repair *repair)
{
    struct stat status;
    int fd, result = 0;

    if (repair->scanned >= WB_REPAIR_MAX_ENTRIES)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    repair->scanned++;
    fd = repair_open_relative_directory(data_fd, relative, repair->device);
    if (fd == -ENOENT)
    {
        repair->skipped++;
        return 0;
    }
    if (fd == -EXDEV)
    {
        repair->skipped++;
        return 0;
    }
    if (fd < 0)
    {
        result = fd;
        goto failed;
    }
    if (fstat(fd, &status))
    {
        result = -errno;
        close(fd);
        goto failed;
    }
    if (status.st_dev != repair->device)
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    result = repair_apply_mode(fd, &status, repair);
    if (recursive)
    {
        int scan_result = repair_scan_directory(fd, 0, repair);
        if (!result) result = scan_result;
    }
    close(fd);
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_all(struct wb_permission_repair *repair)
{
    static const struct
    {
        const char *relative;
        int recursive;
    } roots[] = {
        {"forever", 0},
        {"forever/prefix", 1},
        {"forever/home", 1},
        {"forever/radv-shader-cache", 1},
        {"homebrew/PPSA99030", 0},
        {"homebrew/PPSA99030/wine", 1},
        {"prosperostore/previous/PPSA99030", 0},
        {"prosperostore/previous/PPSA99030/wine", 1},
    };
    int data_fd = repair_open_data_root(&repair->device);

    if (data_fd < 0)
    {
        repair->failed++;
        repair->result = data_fd;
        return data_fd;
    }
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    {
        int result = repair_root(
            data_fd, roots[i].relative, roots[i].recursive, repair);
        if (result == -E2BIG) break;
    }
    close(data_fd);
    return repair->result;
}

static int repair_permissions(void)
{
    struct wb_permission_repair repair = {0};
    struct timeval before, after;
    long long elapsed_ms;
    int result;

    gettimeofday(&before, NULL);
    result = repair_all(&repair);
    gettimeofday(&after, NULL);
    elapsed_ms = (after.tv_sec - before.tv_sec) * 1000LL +
                 (after.tv_usec - before.tv_usec) / 1000;
    say("permission repair scanned=%u changed=%u skipped=%u failed=%u ms=%lld "
        "uid=%u:%u effective=%u:%u result=%d",
        repair.scanned, repair.changed, repair.skipped, repair.failed, elapsed_ms,
        (unsigned)getuid(), (unsigned)getgid(),
        (unsigned)geteuid(), (unsigned)getegid(), result);
    return result;
}

#ifndef WB_STARTUP_REPAIR
#define WB_STARTUP_REPAIR repair_permissions
#endif
#ifndef WB_STARTUP_WRITE_TOKEN
#define WB_STARTUP_WRITE_TOKEN write_token
#endif

static int broker_prepare_startup(void)
{
    WB_STARTUP_REPAIR();
    return WB_STARTUP_WRITE_TOKEN();
}

/* The directory holding a name below the user data root, and in leaf the
 * name's last component; or -errno. The title sends wineserver's canonical
 * (realpath) names, which hold no link, so no component is followed if it is
 * one: a link planted below the root cannot redirect a request outside it.
 * The broker's own files (token, lock) are not the title's. */
static int open_user_parent(const char *path, char leaf[WB_MAX_PATH + 1])
{
    static const char own[] = ".writebroker";
    size_t length = wb_path_root(path);
    int root;

    if (!length || path[length - 1] != '/') return -EACCES;
    if (!strncmp(path + length, own, sizeof(own) - 1)) return -EACCES;
    if ((root = menu_open_prefix_root(WB_WRITE_ROOT)) < 0) return root;
    return menu_open_remove_parent(root, path + length - 1, leaf);
}

/* An existing regular file below the user data root opened for writing, or
 * -errno. O_NONBLOCK keeps a FIFO from stalling the serving thread; only a
 * regular file is accepted. */
static int open_user_file(const char *path)
{
    char leaf[WB_MAX_PATH + 1];
    struct stat st;
    int parent, fd;

    if ((parent = open_user_parent(path, leaf)) < 0) return parent;
    fd = openat(parent, leaf, O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = -errno;
    close(parent);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode)))
    {
        close(fd);
        return -EINVAL;
    }
    return fd;
}

static int serve_write(int client, const struct wb_write *request, unsigned char *buffer)
{
    char path[WB_MAX_PATH + 1];
    struct wb_reply reply = {.magic = WB_MAGIC};
    uint32_t remaining = request->length;
    uint64_t offset = request->offset;
    int fd = -1, result = 0;

    if (request->path_length == 0 || request->path_length > WB_MAX_PATH || request->length > WB_MAX_WRITE)
        return -1;  /* malformed: drop the connection */
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;

    /* the user data root is the same path for the title and this payload */
    if ((fd = open_user_file(path)) < 0) result = fd;

    while (remaining)
    {
        uint32_t chunk = remaining < WB_CHUNK ? remaining : WB_CHUNK, put = 0;

        if (transfer(client, buffer, chunk, 0)) { if (fd >= 0) close(fd); return -1; }
        while (!result && put < chunk)
        {
            ssize_t count = pwrite(fd, buffer + put, chunk - put, (off_t)offset + put);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { result = count ? -errno : -EIO; break; }
            put += (uint32_t)count;
        }
        offset += chunk;
        remaining -= chunk;
    }
    if (fd >= 0) close(fd);
    if (result && remaining) discard(client, buffer, remaining);
    reply.result = result ? result : (int32_t)request->length;
    if (result) say("write refused %d: %s", result, path);
    else
    {
        unsigned long long writes, bytes;
        pthread_mutex_lock(&stats_lock);
        writes = ++served_writes;
        bytes = served_bytes += request->length;
        pthread_mutex_unlock(&stats_lock);
        if (!(writes % 4096)) say("served %llu writes, %llu MiB", writes, bytes >> 20);
    }
    return transfer(client, &reply, sizeof(reply), 1);
}

static int serve_statfs(int client, const struct wb_write *request)
{
    char path[WB_MAX_PATH + 1];
    struct wb_statfs_reply reply = {.magic = WB_MAGIC};
    struct statfs info;

    if (!request->path_length || request->path_length > WB_MAX_PATH ||
        request->offset || request->length || request->reserved)
        return -1;
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;
    if (!wb_path_root(path)) reply.result = -EACCES;
    else if (statfs(path, &info)) reply.result = -errno;
    else
    {
        reply.bsize = info.f_bsize;
        reply.blocks = info.f_blocks;
        reply.bfree = info.f_bfree;
        reply.bavail = info.f_bavail > 0 ? (uint64_t)info.f_bavail : 0;
    }
    return transfer(client, &reply, sizeof(reply), 1);
}

/* Namespace requests act for the title, which the console throttles, and
 * never do what it could not: the parent must let the title's uid/gid add
 * and remove entries (by its mode bits; in a sticky directory only the
 * owner of the entry or directory removes it). entry is the entry removed
 * or replaced, if any. */
static int namespace_access(int parent, const struct stat *entry, const struct wb_namespace *ns)
{
    struct stat st;
    mode_t need;

    if (fstat(parent, &st)) return -errno;
    if (!ns->uid) return 0;
    need = st.st_uid == ns->uid ? S_IWUSR | S_IXUSR
         : st.st_gid == ns->gid ? S_IWGRP | S_IXGRP : S_IWOTH | S_IXOTH;
    if ((st.st_mode & need) != need) return -EACCES;
    if (entry && (st.st_mode & S_ISVTX) && st.st_uid != ns->uid && entry->st_uid != ns->uid)
        return -EACCES;
    return 0;
}

/* Whether st, the entry named path in parent, is the one the title named.
 * The title's sandbox sees /data through another mount than this payload, so
 * its st_dev differs. Assumption, not yet confirmed on the console: that
 * mount passes inode numbers through, as FreeBSD nullfs does (it reports
 * only its own fsid). So the entry is matched by inode number and file type,
 * and must be on its parent's filesystem in this payload's own view, where
 * inode numbers are unique; the title's dev is only logged. Confinement does
 * not rest on this: it is the user data root, the O_NOFOLLOW component opens
 * and namespace_access. A mismatch is -ESTALE, which the title answers by
 * making the call itself: slow, never wrong. */
static int namespace_same(int parent, const struct stat *st, const struct wb_namespace *ns,
                          const char *path)
{
    static unsigned logged;
    struct stat directory = {0};

    if (!fstat(parent, &directory) && st->st_dev == directory.st_dev &&
        (uint64_t)st->st_ino == ns->ino && (st->st_mode & S_IFMT) == (ns->mode & S_IFMT))
        return 1;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 64)
        say("namespace identity title dev=%llx ino=%llx type=%o; broker dev=%llx ino=%llx "
            "type=%o parent dev=%llx: %s",
            (unsigned long long)ns->dev, (unsigned long long)ns->ino, (unsigned)(ns->mode & S_IFMT),
            (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
            (unsigned)(st->st_mode & S_IFMT), (unsigned long long)directory.st_dev, path);
    return 0;
}

/* A new regular file or directory leaf in parent, as the title would have
 * made it: mode & 0777 whatever this process's umask, owned by the title's
 * uid (the group comes from the parent, as for the title's own). One the
 * title could not own is removed again. */
static int namespace_create(int parent, const char *leaf, const struct wb_namespace *ns, int directory)
{
    mode_t mode = ns->mode & 0777;
    struct stat made, named;
    int fd, result = 0;

    if (directory)
    {
        if (mkdirat(parent, leaf, mode)) return -errno;
        fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    else
    {
        fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
        /* the title opens what is there, as its own open would have */
        if (fd < 0 && errno == EEXIST && !(ns->flags & WB_NS_EXCL)) return 0;
    }
    if (fd < 0) return -errno;
    if (fchown(fd, (uid_t)ns->uid, (gid_t)-1) || fchmod(fd, mode))
    {
        result = -errno;
        if (!fstat(fd, &made) && !fstatat(parent, leaf, &named, AT_SYMLINK_NOFOLLOW) &&
            made.st_dev == named.st_dev && made.st_ino == named.st_ino)
            unlinkat(parent, leaf, directory ? AT_REMOVEDIR : 0);
    }
    close(fd);
    return result;
}

/* ENOENT, EEXIST and ENOTEMPTY come only from the entry named: a missing
 * one, or once its identity matched; the title takes them as the answer. */
static int namespace_unlink(int parent, const char *leaf, const struct wb_namespace *ns,
                            const char *path)
{
    struct stat st;
    int result;

    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW)) return -errno;
    if (!namespace_same(parent, &st, ns, path)) return -ESTALE;
    if ((result = namespace_access(parent, &st, ns))) return result;
    return unlinkat(parent, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) ? -errno : 0;
}

static int namespace_rename(int from, const char *from_leaf, int to, const char *to_leaf,
                            const struct wb_namespace *ns, const char *path)
{
    struct stat st, target;
    int result;

    if (fstatat(from, from_leaf, &st, AT_SYMLINK_NOFOLLOW)) return -errno;
    if (!namespace_same(from, &st, ns, path)) return -ESTALE;
    if ((result = namespace_access(from, &st, ns))) return result;
    if (!fstatat(to, to_leaf, &target, AT_SYMLINK_NOFOLLOW))
    {
        if (!(ns->flags & WB_NS_REPLACE)) return -EEXIST;
        if (!S_ISREG(target.st_mode)) return -EACCES;
        if ((result = namespace_access(to, &target, ns))) return result;
    }
    else if (errno != ENOENT) return -errno;
    else if ((result = namespace_access(to, NULL, ns))) return result;
    return renameat(from, from_leaf, to, to_leaf) ? -errno : 0;
}

/* WB_CREATE_MAGIC, WB_MKDIR_MAGIC, WB_UNLINK_MAGIC, WB_RENAME_MAGIC */
static int serve_namespace(int client, const struct wb_write *request)
{
    static unsigned refusals;
    char path[WB_MAX_PATH + 1], target[WB_MAX_PATH + 1];
    char leaf[WB_MAX_PATH + 1], target_leaf[WB_MAX_PATH + 1];
    struct wb_reply reply = {.magic = WB_MAGIC};
    struct wb_namespace ns;
    int renaming = request->magic == WB_RENAME_MAGIC, parent = -1, target_parent = -1;
    uint32_t target_length = renaming ? request->length : 0;

    if (!request->path_length || request->path_length > WB_MAX_PATH || request->offset ||
        request->reserved || request->length != target_length ||
        (renaming && (!target_length || target_length > WB_MAX_PATH)))
        return -1;
    if (transfer(client, &ns, sizeof(ns), 0) || transfer(client, path, request->path_length, 0) ||
        transfer(client, target, target_length, 0))
        return -1;
    path[request->path_length] = target[target_length] = 0;
    if (memchr(path, 0, request->path_length) || memchr(target, 0, target_length)) return -1;

    if ((ns.flags & ~(uint32_t)(WB_NS_EXCL | WB_NS_REPLACE)) ||
        ((request->magic == WB_UNLINK_MAGIC || renaming) && !ns.ino))
        reply.result = -EINVAL;
    else if ((parent = open_user_parent(path, leaf)) < 0)
        reply.result = parent;
    else if (renaming && (target_parent = open_user_parent(target, target_leaf)) < 0)
        reply.result = target_parent;
    else if (request->magic == WB_UNLINK_MAGIC)
        reply.result = namespace_unlink(parent, leaf, &ns, path);
    else if (renaming)
        reply.result = namespace_rename(parent, leaf, target_parent, target_leaf, &ns, path);
    else if (!(reply.result = namespace_access(parent, NULL, &ns)))
        reply.result = namespace_create(parent, leaf, &ns, request->magic == WB_MKDIR_MAGIC);
    if (parent >= 0) close(parent);
    if (target_parent >= 0) close(target_parent);
    /* missing, existing and non-empty entries are the title's everyday answers */
    if (reply.result && reply.result != -ENOENT && reply.result != -EEXIST &&
        reply.result != -ENOTEMPTY && __atomic_fetch_add(&refusals, 1, __ATOMIC_RELAXED) < 64)
        say("namespace refused %d %08x: %s%s%s", reply.result, request->magic, path,
            renaming ? " -> " : "", target);
    return transfer(client, &reply, sizeof(reply), 1);
}

static int namespace_magic(uint32_t magic)
{
    return magic == WB_CREATE_MAGIC || magic == WB_MKDIR_MAGIC ||
           magic == WB_UNLINK_MAGIC || magic == WB_RENAME_MAGIC;
}

/* A regular file of at most limit bytes into buffer: its size, or -errno */
static ssize_t read_whole(const char *path, unsigned char *buffer, size_t limit)
{
    struct stat st;
    ssize_t done = 0, got = 0;
    int fd = open(path, O_RDONLY), error = 0;

    if (fd < 0) return -errno;
    if (fstat(fd, &st)) error = errno;
    else if (!S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit) error = EFBIG;
    while (!error && done < st.st_size && (got = read(fd, buffer + done, (size_t)(st.st_size - done))) > 0)
        done += got;
    if (!error && done != st.st_size) error = got < 0 ? errno : EIO;
    close(fd);
    return error ? -error : done;
}

/* Copy WB_ICON_FILE over each of the home screen's copies of the title's
 * icon that exists and differs (ShadowMountPlus made them when it
 * registered the title; the shell shows a new one after a restart).
 * buffer and other hold WB_CHUNK bytes. How many were replaced, or -errno. */
static int install_icon(unsigned char *buffer, unsigned char *other)
{
    static const char *const copies[] = {
        "/user/app/" WB_TITLE_ID "/icon0.png",
        "/user/app/" WB_TITLE_ID "/sce_sys/icon0.png",
        "/user/appmeta/" WB_TITLE_ID "/icon0.png",
    };
    static const unsigned char png[16] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    static const unsigned char size512[8] = {0, 0, 2, 0, 0, 0, 2, 0};
    char temporary[128];
    ssize_t size = read_whole(WB_ICON_FILE, buffer, WB_CHUNK), existing;
    int replaced = 0, present = 0;

    if (size < 0) return (int)size;
    if (size < 24 || memcmp(buffer, png, sizeof(png)) || memcmp(buffer + 16, size512, sizeof(size512)))
        return -EINVAL;
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++)
    {
        int fd, ok;

        if ((existing = read_whole(copies[i], other, WB_CHUNK)) == -ENOENT) continue;
        ++present;
        if (existing == size && !memcmp(buffer, other, (size_t)size)) continue;
        snprintf(temporary, sizeof(temporary), "%s.new", copies[i]);
        if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -errno;
        ok = write(fd, buffer, (size_t)size) == size && !fchmod(fd, 0666);
        ok = !close(fd) && ok;
        if (!ok || rename(temporary, copies[i]))
        {
            int error = errno ? errno : EIO;
            unlink(temporary);
            return -error;
        }
        ++replaced;
    }
    say("home-screen icon: %d of %d copies replaced", replaced, present);
    return replaced;
}

/* The token file is the credential: whoever can read it is trusted. A broker
 * whose user data folder was replaced (new token file) used to refuse every
 * hello until the console restarted, and a replacement could not take the
 * port. On a mismatch, adopt the file's current token if the client offers
 * exactly that. */
static int token_accepts(const unsigned char offered[WB_TOKEN_BYTES])
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned char current[WB_TOKEN_BYTES];
    int fd, ok;

    pthread_mutex_lock(&lock);
    ok = !memcmp(offered, token, sizeof(token));
    if (!ok && (fd = open(token_file, O_RDONLY)) >= 0)
    {
        ok = read(fd, current, sizeof(current)) == (ssize_t)sizeof(current) &&
             !memcmp(offered, current, sizeof(current));
        close(fd);
        if (ok)
        {
            memcpy(token, current, sizeof(token));
            say("adopted the current token file");
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static void *serve(void *arg)
{
    int client = (int)(intptr_t)arg;
    struct wb_hello hello;
    struct wb_reply reply = {.magic = WB_MAGIC};
    struct wb_write request;
    unsigned char *buffer = malloc(WB_CHUNK);

    if (!buffer || transfer(client, &hello, sizeof(hello), 0) || hello.magic != WB_MAGIC ||
        !hello.version || !token_accepts(hello.token))
    {
        say("connection refused (hello)");
        goto done;
    }
    /* another version may only ask this broker to exit */
    if (hello.version != WB_VERSION) reply.result = -EPROTONOSUPPORT;
    if (transfer(client, &reply, sizeof(reply), 1)) goto done;
    while (!transfer(client, &request, sizeof(request), 0))
    {
        if (request.magic == WB_QUIT_MAGIC)
        {
            /* a newer broker holding the same token takes over the port */
            say("replaced by a new broker; exiting");
            exit(0);
        }
        if (!reply.result && request.magic == WB_MENU_CAPS_MAGIC)
        {
            struct wb_reply answer = {.magic = WB_MAGIC, .result = WB_MENU_CAPS_REMOVE};
            if (request.path_length || request.offset || request.length || request.reserved) answer.result = -EINVAL;
            if (transfer(client, &answer, sizeof(answer), 1)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_MENU_REMOVE_MAGIC)
        {
            if (serve_menu_remove(client, &request)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_ICON_MAGIC)
        {
            struct wb_reply answer = {.magic = WB_MAGIC, .result = -ENOMEM};
            unsigned char *other = malloc(WB_CHUNK);

            if (request.path_length || request.length) answer.result = -EINVAL;
            else if (other) answer.result = install_icon(buffer, other);
            free(other);
            if (transfer(client, &answer, sizeof(answer), 1)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_STATFS_MAGIC)
        {
            if (serve_statfs(client, &request)) break;
            continue;
        }
        if (!reply.result && namespace_magic(request.magic))
        {
            if (serve_namespace(client, &request)) break;
            continue;
        }
        if (reply.result || request.magic != WB_MAGIC || serve_write(client, &request, buffer)) break;
    }
done:
    free(buffer);
    close(client);
    return NULL;
}

/* Known limitation retained from the original protocol: the token is 0644 in
 * the normally 0777 user-data directory. It authenticates cooperative title
 * clients but is not secret from other local identities that can traverse
 * /data/forever; changing the credential scheme is outside this repair. */
static int write_token(void)
{
    static const char name[] = WB_TOKEN_FILE, temporary[] = WB_TOKEN_FILE ".new";
    int fd = open("/dev/urandom", O_RDONLY), ok;

    ok = fd >= 0 && read(fd, token, sizeof(token)) == (ssize_t)sizeof(token);
    if (fd >= 0) close(fd);
    if (!ok) return -1;
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
    ok = write(fd, token, sizeof(token)) == (ssize_t)sizeof(token);
    fchmod(fd, 0644);
    close(fd);
    return ok && !rename(temporary, name) ? 0 : -1;
}

/* What an accept() error means for the listener: -1 it is gone, 0 retry now
 * (only that connection failed), 1 retry after a pause (descriptors, buffers
 * or memory are short, and retrying at once would spin). */
static int accept_error_action(int error)
{
    switch (error)
    {
    case EBADF: case EINVAL: case ENOTSOCK: case EOPNOTSUPP: return -1;
    case EINTR: case ECONNABORTED: case EPROTO: return 0;
    default: return 1;
    }
}

/* What holds the port, from replace_running */
enum
{
    WB_RUNNING_NONE,     /* nothing accepts */
    WB_RUNNING_CURRENT,  /* a broker of this version answers: left running */
    WB_RUNNING_REPLACED, /* another version answered and was asked to exit */
    WB_RUNNING_OTHER,    /* something accepts, but no broker answers */
};

/* Ask a broker of another version on the port to exit: it trusts whoever can
 * read its token. An older one answers only its own version's hello (version
 * 1 closes the connection on any other), so step down until one answers. A
 * broker of this version that accepts the token is left alone. Without a
 * token file there is no token to invalidate: write this broker's own, which
 * a running broker adopts. */
static int replace_running(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WB_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct wb_hello hello = {.magic = WB_MAGIC};
    struct wb_write quit = {.magic = WB_QUIT_MAGIC};
    struct wb_reply reply;
    struct timeval timeout = {.tv_sec = 1};
    int fd = open(WB_TOKEN_FILE, O_RDONLY), ok, running = WB_RUNNING_NONE;

    ok = fd >= 0 && read(fd, hello.token, sizeof(hello.token)) == (ssize_t)sizeof(hello.token);
    if (fd >= 0) close(fd);
    if (!ok)
    {
        if (WB_STARTUP_WRITE_TOKEN()) return WB_RUNNING_NONE;
        memcpy(hello.token, token, sizeof(hello.token));
    }
    for (hello.version = WB_VERSION; hello.version; hello.version--)
    {
        int error;

        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) break;
        /* a program that accepts and stays silent must not hold this one */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)))
        {
            close(fd);
            break;
        }
        running = WB_RUNNING_OTHER;
        if (!transfer(fd, &hello, sizeof(hello), 1) && !transfer(fd, &reply, sizeof(reply), 0))
        {
            if (reply.magic == WB_MAGIC && hello.version == WB_VERSION && !reply.result)
                running = WB_RUNNING_CURRENT;
            else if (reply.magic == WB_MAGIC)
            {
                transfer(fd, &quit, sizeof(quit), 1);
                say("asked the running broker to exit (version %u hello, reply %d)",
                    hello.version, reply.result);
                running = WB_RUNNING_REPLACED;
            }
            close(fd);
            break;
        }
        error = errno;
        close(fd);
        /* a closed connection is an older broker refusing this version */
        if (error != EPIPE && error != ECONNRESET) break;
    }
    return running;
}

/* Make this the only broker: take WB_LOCK_FILE, then the port. The lock lasts
 * as long as this process, so a second copy never repairs, writes the token
 * or asks the first to quit, and a copy that cannot get the port never
 * replaces the running broker's token. Where locks are unavailable the port
 * alone decides. 0: this broker owns the port. 1: a broker of this version
 * answers or is starting. -1: not started (logged). */
static int claim_broker(int listener, const struct sockaddr_in *address)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int fd = open(WB_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    int locked = 0, running = WB_RUNNING_NONE, error = 0, attempt;

    for (attempt = 0;; attempt++)
    {
        if (!locked && (fd < 0 || fcntl(fd, F_SETLK, &lock)))
        {
            error = errno;
            if (fd < 0 || (error != EAGAIN && error != EACCES))
            {
                say("no lock %s errno=%d; the port alone decides", WB_LOCK_FILE, error);
                locked = 1;
            }
        }
        else locked = 1;
        if (locked)
        {
            if (!bind(listener, (const struct sockaddr *)address, sizeof(*address)))
                return 0; /* fd stays open: closing it would drop the lock */
            if ((error = errno) != EADDRINUSE) break;
        }
        if (!attempt)
        {
            running = replace_running();
            if (running == WB_RUNNING_CURRENT || (!locked && running != WB_RUNNING_REPLACED))
            {
                say("a broker of this version %s; exiting",
                    running == WB_RUNNING_CURRENT ? "already answers" : "is starting");
                if (fd >= 0) close(fd);
                return 1;
            }
        }
        /* an older broker was asked to exit: retry for 2 s */
        if (attempt == 10) break;
        usleep(200000);
    }
    if (fd >= 0) close(fd);
    if (running == WB_RUNNING_OTHER && error == EADDRINUSE)
        say("not started: port %d is held by another program (no write broker answers there)",
            WB_PORT);
    else
        say("not started: %s errno=%d", locked ? "port" : WB_LOCK_FILE, error);
    return -1;
}

int main(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WB_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1, claimed, last_error = 0;

    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
    {
        say("not started: socket errno=%d", errno);
        return 1;
    }
    if ((claimed = claim_broker(listener, &address))) return claimed < 0;
    /* only now, owning the port, repair and replace the token */
    if (broker_prepare_startup())
    {
        say("not started: cannot write %s errno=%d", WB_TOKEN_FILE, errno);
        return 1;
    }
    if (listen(listener, 64))
    {
        say("not started: listen errno=%d", errno);
        return 1;
    }
    say("listening on 127.0.0.1:%d for %s identity=%u:%u effective=%u:%u",
        WB_PORT, WB_USER_ROOT, (unsigned)getuid(), (unsigned)getgid(),
        (unsigned)geteuid(), (unsigned)getegid());
    for (;;)
    {
        int client = accept(listener, NULL, NULL);
        struct timeval timeout = {.tv_sec = 30};
        pthread_t thread;

        if (client < 0)
        {
            int error = errno, action = accept_error_action(error);

            if (action < 0) { say("accept errno=%d", error); break; }
            if (action && error != last_error) say("accept errno=%d; retrying", error);
            last_error = error;
            if (action) usleep(100000);
            continue;
        }
        last_error = 0;
        /* an idle title connection may wait as long as it likes; a stuck reply may not */
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (pthread_create(&thread, NULL, serve, (void *)(intptr_t)client)) { close(client); continue; }
        pthread_detach(thread);
    }
    close(listener);
    return 1;
}
