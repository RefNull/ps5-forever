/* The title's write-broker probe and add-on worker port wait, extracted from
 * native-entry.c, against real loopback listeners: a healthy broker or worker
 * is told apart from another program that holds the port. Only the title's
 * nonblocking connect and its log are host stand-ins. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "wine/ps5_writebroker.h"
#include "writebroker-autostart.h"
#include "writebroker-menu.h"
#include "../../addon-worker/protocol.h"

#undef WB_TOKEN_FILE
#define WB_TOKEN_FILE token_path

static char token_path[1024], logged[2048];
static int test_port, failures;

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static int64_t io_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void io_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void io_log(const char *format, ...)
{
    size_t used = strlen(logged);
    va_list args;
    va_start(args, format);
    vsnprintf(logged + used, sizeof(logged) - used, format, args);
    va_end(args);
}

/* Every title port is the scenario's loopback port. */
static int pw_writebroker_connect_port(unsigned int port, int64_t deadline)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)test_port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0), saved;

    (void)deadline;
    if (port != PW_WRITEBROKER_PORT && port != AW_PORT) abort();
    if (fd < 0) abort();
    if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) return fd;
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

#include "title-port-owner.inc"

enum peer { PEER_SILENT, PEER_GARBAGE, PEER_BROKER, PEER_WORKER };

struct server
{
    int listener;
    enum peer peer;
    volatile int stop;
    pthread_t thread;
};

static int receive_all(int fd, void *data, size_t size)
{
    unsigned char *bytes = data;
    while (size)
    {
        ssize_t got = recv(fd, bytes, size, 0);
        if (got <= 0) return -1;
        bytes += got;
        size -= (size_t)got;
    }
    return 0;
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
}

/* One connection as the peer would serve it. */
static void serve_peer(int client, enum peer peer)
{
    unsigned char bytes[64];
    struct wb_reply ok = {.magic = WB_MAGIC}, caps = {.magic = WB_MAGIC, .result = WB_MENU_CAPS_REMOVE};

    if (peer == PEER_GARBAGE && !receive_all(client, bytes, sizeof(struct wb_hello)))
    {
        memset(bytes, 0xff, sizeof(struct wb_reply));
        send(client, bytes, sizeof(struct wb_reply), MSG_NOSIGNAL);
    }
    if (peer == PEER_BROKER && !receive_all(client, bytes, sizeof(struct wb_hello)) &&
        send(client, &ok, sizeof(ok), MSG_NOSIGNAL) == sizeof(ok) &&
        !receive_all(client, bytes, sizeof(struct wb_write)))
        send(client, &caps, sizeof(caps), MSG_NOSIGNAL);
    if (peer == PEER_WORKER && !receive_all(client, bytes, AW_HELLO_BYTES))
    {
        memset(bytes, 0, AW_CHALLENGE_REPLY_BYTES);
        put_u32(bytes, AW_MAGIC);
        put_u32(bytes + 4, AW_VERSION);
        send(client, bytes, AW_CHALLENGE_REPLY_BYTES, MSG_NOSIGNAL);
    }
    close(client);
}

static void *server_main(void *opaque)
{
    struct server *server = opaque;
    while (!server->stop)
    {
        struct pollfd wait = {.fd = server->listener, .events = POLLIN};
        if (poll(&wait, 1, 50) <= 0) continue;
        serve_peer(accept(server->listener, NULL, NULL), server->peer);
    }
    return NULL;
}

static void start_server(struct server *server, enum peer peer)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    int one = 1;

    memset(server, 0, sizeof(*server));
    server->peer = peer;
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0 ||
        setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(server->listener, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(server->listener, (struct sockaddr *)&address, &size) ||
        listen(server->listener, 64)) abort();
    test_port = ntohs(address.sin_port);
    /* a silent program accepts in the kernel only and never answers */
    if (peer != PEER_SILENT && pthread_create(&server->thread, NULL, server_main, server)) abort();
    logged[0] = 0;
}

static void stop_server(struct server *server)
{
    server->stop = 1;
    if (server->peer != PEER_SILENT) pthread_join(server->thread, NULL);
    close(server->listener);
}

static int probe(int *error)
{
    return pw_writebroker_probe(NULL, io_now_ms() + PW_WRITEBROKER_PROBE_MS, error);
}

static void broker_probe_tests(void)
{
    struct server server;
    int error, result;

    start_server(&server, PEER_BROKER);
    check(probe(&error) == 1 && !error, "the probe recognises a healthy write broker");
    stop_server(&server);

    start_server(&server, PEER_SILENT);
    result = probe(&error);
    check(result < 0 && error == EADDRINUSE,
          "the probe reports a silent listener on the broker port as another program");
    check(strstr(logged, "held by another program") != NULL,
          "the title logs that another program holds the broker port");
    stop_server(&server);

    start_server(&server, PEER_GARBAGE);
    result = probe(&error);
    check(result < 0 && error == EADDRINUSE,
          "the probe does not mistake a foreign answer for an older broker");
    stop_server(&server);

    start_server(&server, PEER_SILENT);
    stop_server(&server);
    check(probe(&error) == 0 && error == ECONNREFUSED, "the probe reports a free broker port");
}

static void worker_port_tests(void)
{
    struct server server;

    start_server(&server, PEER_WORKER);
    check(pw_addon_worker_wait_stopped() == -ETIMEDOUT &&
          strstr(logged, "another program") == NULL,
          "a worker that still answers is waited for, not reported as another program");
    stop_server(&server);

    start_server(&server, PEER_SILENT);
    check(pw_addon_worker_wait_stopped() == -EADDRINUSE,
          "a silent listener on the worker port is reported as another program");
    check(strstr(logged, "held by another program") != NULL,
          "the title logs that another program holds the worker port");
    stop_server(&server);

    start_server(&server, PEER_SILENT);
    stop_server(&server);
    check(!pw_addon_worker_wait_stopped(), "a free worker port needs no wait");
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    int fd;

    signal(SIGPIPE, SIG_IGN);
    snprintf(token_path, sizeof(token_path), "%s/title-port-owner-XXXXXX",
             base && *base ? base : "/tmp");
    if ((fd = mkstemp(token_path)) < 0 || write(fd, "title-port-token", 16) != 16) abort();
    close(fd);
    broker_probe_tests();
    worker_port_tests();
    unlink(token_path);
    if (failures) return 1;
    puts("title port owner: healthy broker/worker vs another program on the port PASS");
    return 0;
}
