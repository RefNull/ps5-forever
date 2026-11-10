/* Unpack mode: the product's download is a zip holding exactly one file, the
 * launcher itself, which goes to the launcher path. Nothing is run. */
#ifndef FOREVER_UNPACK_H
#define FOREVER_UNPACK_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sha256.h"

/* In-memory zip reading only: no stdio, clock or writer imports in the title.
 * forever-miniz.c compiles miniz with these settings. */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../../addon-worker/third_party/miniz/miniz.h"

/* Both refuse surprises; the bootstrapper zip is a few MiB. */
#define FOREVER_UNPACK_MAX_ZIP_BYTES (64u * 1024u * 1024u)
#define FOREVER_UNPACK_MAX_MEMBER_BYTES (64u * 1024u * 1024u)

/* A plain Windows file name: no path, drive, parent or control characters. */
static inline int forever_unpack_plain_name(const char *name)
{
    size_t length = strlen(name);
    if (!length || length > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
        name[length - 1] == '.' || name[length - 1] == ' ') return 0;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7f || strchr("<>:\"/\\|?*", c)) return 0;
    }
    return 1;
}

static inline int forever_unpack_sha256(const void *data, size_t size, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    struct sha256_context hash;
    unsigned char digest[32];
    sha256_init(&hash);
    sha256_update(&hash, data, size);
    sha256_final(&hash, digest);
    if (!expected || strlen(expected) != 64) return EINVAL;
    for (unsigned i = 0; i < 32; i++)
        if (hex[digest[i] >> 4] != expected[i * 2] || hex[digest[i] & 15] != expected[i * 2 + 1])
            return EINVAL;
    return 0;
}

/* Creates the missing parents of an absolute path. */
static inline int forever_unpack_parents(const char *path)
{
    char directory[1024];
    size_t length = strlen(path);
    if (path[0] != '/' || length >= sizeof(directory)) return ENAMETOOLONG;
    memcpy(directory, path, length + 1);
    for (char *p = directory + 1; *p; p++)
    {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(directory, 0777) && errno != EEXIST) return errno;
        *p = '/';
    }
    return 0;
}

struct forever_unpack_sink
{
    int fd, error;
    uint64_t written;
};

/* miniz fails the extraction when this returns less than n. */
static inline size_t forever_unpack_write(void *opaque, mz_uint64 offset, const void *data, size_t n)
{
    struct forever_unpack_sink *sink = opaque;
    size_t done = 0;
    if (offset != sink->written || n > FOREVER_UNPACK_MAX_MEMBER_BYTES - sink->written)
    {
        sink->error = EFBIG;
        return 0;
    }
    while (done < n)
    {
        ssize_t count = write(sink->fd, (const char *)data + done, n - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { sink->error = count < 0 ? errno : EIO; return 0; }
        done += (size_t)count;
    }
    sink->written += n;
    return n;
}

/* Verifies the zip against sha256, requires member to be its only entry and
 * publishes it at destination through destination.part. Zero on success. */
static inline int forever_unpack_zip(const char *zip_path, const char *sha256, const char *member,
                                     const char *destination, const char **step)
{
    mz_zip_archive zip;
    mz_zip_archive_file_stat entry;
    struct forever_unpack_sink sink = {-1, 0, 0};
    struct stat file;
    unsigned char *data = NULL;
    char part[1024];
    size_t size = 0;
    int fd = -1, opened = 0, created = 0, result = 0;

    memset(&zip, 0, sizeof(zip));
    *step = "invalid unpack settings";
    if (!forever_unpack_plain_name(member) || snprintf(part, sizeof(part), "%s.part", destination) >=
                                                  (int)sizeof(part))
        return EINVAL;
    *step = "reading the download";
    if ((fd = open(zip_path, O_RDONLY)) < 0) return errno;
    if (fstat(fd, &file)) { result = errno; goto done; }
    *step = "download is too large";
    if (!S_ISREG(file.st_mode) || file.st_size <= 0 ||
        (uint64_t)file.st_size > FOREVER_UNPACK_MAX_ZIP_BYTES) { result = EFBIG; goto done; }
    *step = "reading the download";
    if (!(data = malloc((size_t)file.st_size))) { result = ENOMEM; goto done; }
    while (size < (size_t)file.st_size)
    {
        ssize_t count = read(fd, data + size, (size_t)file.st_size - size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count < 0 ? errno : EIO; goto done; }
        size += (size_t)count;
    }
    close(fd);
    fd = -1;
    *step = "download SHA-256 mismatch";
    if ((result = forever_unpack_sha256(data, size, sha256))) goto done;
    *step = "download is not a zip";
    if (!mz_zip_reader_init_mem(&zip, data, size, 0)) { result = EINVAL; goto done; }
    opened = 1;
    *step = "zip does not hold exactly the expected file";
    if (mz_zip_reader_get_num_files(&zip) != 1 || !mz_zip_reader_file_stat(&zip, 0, &entry) ||
        entry.m_is_directory || entry.m_is_encrypted || !entry.m_is_supported ||
        !forever_unpack_plain_name(entry.m_filename) || strcmp(entry.m_filename, member))
    { result = EINVAL; goto done; }
    *step = "zip file is too large";
    if (entry.m_uncomp_size > FOREVER_UNPACK_MAX_MEMBER_BYTES) { result = EFBIG; goto done; }
    *step = "creating the launcher folder";
    if ((result = forever_unpack_parents(destination))) goto done;
    *step = "writing the launcher";
    if ((sink.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) { result = errno; goto done; }
    created = 1;
    if (!mz_zip_reader_extract_to_callback(&zip, 0, forever_unpack_write, &sink, 0))
    {
        if (sink.error == EFBIG) *step = "zip file is too large";
        else if (!sink.error) *step = "zip file is damaged";
        result = sink.error ? sink.error : EINVAL;
        goto done;
    }
    if (sink.written != entry.m_uncomp_size) { *step = "zip file is damaged"; result = EINVAL; goto done; }
    *step = "syncing the launcher";
    if (fsync(sink.fd)) { result = errno; goto done; }
    result = close(sink.fd);
    sink.fd = -1;
    if (result) { result = errno; goto done; }
    *step = "publishing the launcher";
    if (rename(part, destination)) { result = errno; goto done; }
    result = 0;
done:
    if (fd >= 0) close(fd);
    if (sink.fd >= 0) close(sink.fd);
    if (result && created) unlink(part);
    if (opened) mz_zip_reader_end(&zip);
    free(data);
    return result;
}

#endif
