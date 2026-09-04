/*
 * 86Box memory dump server.
 *
 * A tiny socket server that answers "dump guest physical memory" requests
 * from a dedicated thread.  Unlike the GDB stub, it NEVER touches the CPU
 * state: reads go straight to the emulated memory arrays (the ram-backed
 * pages resolve through readlookup2 to a direct pointer; device-mapped
 * pages such as the video window go through the registered mapping's byte
 * read).  The caller accepts tearing, so the data races against the
 * emulation thread are benign.
 *
 * Protocol (one request per connection):
 *   request:  "<hex-address>:<hex-length>\n"
 *   response: "<length*2 hex characters>\n"
 *
 * The gdb-stub port must stay untouched: this is the CPU-suspension-free
 * path the diagnostics tool uses to poll the guest screen.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <86box/86box.h>
#include <cpu.h>
#include <86box/mem.h>
#include <86box/memdump.h>
#include <86box/thread.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

static int       memdump_socket = -1;
static volatile int memdump_running = 0;

/* Read one guest-physical byte without involving the CPU. */
static uint8_t
memdump_read_byte(uint32_t addr)
{
    uintptr_t      ptr = readlookup2[addr >> 12];
    mem_mapping_t *map;

    if (ptr != (uintptr_t) LOOKUP_INV)
        return *(uint8_t *) (ptr + addr);

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
        return map->read_b(addr, map->priv);

    return 0xff;
}

static void
memdump_server_thread(void *priv)
{
    struct sockaddr_in client_addr;
    socklen_t          client_len = sizeof(client_addr);
    char               request[64];
    char               reply[4096];
    char              *buf = NULL;
    uint32_t           addr;
    uint32_t           len;
    ssize_t            n;

    (void) priv;

    while (memdump_running) {
        int conn = accept(memdump_socket, (struct sockaddr *) &client_addr, &client_len);
        if (conn < 0) {
            if (!memdump_running)
                break;
            continue;
        }

        n = 0;
        while (n < (ssize_t) (sizeof(request) - 1)) {
            ssize_t r = recv(conn, request + n, 1, 0);
            if (r <= 0)
                break;
            n += r;
            if (request[n - 1] == '\n')
                break;
        }
        request[n] = '\0';

        addr = 0;
        len  = 0;
        if (sscanf(request, "%x:%x", &addr, &len) == 2 && len <= 0x1000) {
            if (len * 2 + 1 <= sizeof(reply)) {
                for (uint32_t i = 0; i < len; i++)
                    sprintf(reply + i * 2, "%02x", memdump_read_byte(addr + i));
                send(conn, reply, (int) (len * 2), 0);
            } else {
                buf = (char *) malloc((size_t) len * 2 + 1);
                if (buf != NULL) {
                    for (uint32_t i = 0; i < len; i++)
                        sprintf(buf + i * 2, "%02x", memdump_read_byte(addr + i));
                    send(conn, buf, (int) (len * 2), 0);
                    free(buf);
                }
            }
        }
        send(conn, "\n", 1, 0);

#ifdef _WIN32
        closesocket(conn);
#else
        close(conn);
#endif
    }
}

void
memdump_init(void)
{
    struct sockaddr_in bind_addr;

    if (memdump_port <= 0)
        return;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    memdump_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (memdump_socket == -1) {
        pclog("MemDump: failed to create socket\n");
        return;
    }

    int yes = 1;
    setsockopt(memdump_socket, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
               (const char *) &yes,
#else
               &yes,
#endif
               sizeof(yes));

    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port        = htons((uint16_t) memdump_port);

    if (bind(memdump_socket, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) == -1) {
        pclog("MemDump: failed to bind on port %d\n", memdump_port);
        return;
    }
    if (listen(memdump_socket, 1) == -1) {
        pclog("MemDump: failed to listen on port %d\n", memdump_port);
        return;
    }

    memdump_running = 1;
    pclog("MemDump: Listening on port %d\n", memdump_port);
    thread_create(memdump_server_thread, NULL);
}

void
memdump_close(void)
{
    if (memdump_socket < 0)
        return;

    memdump_running = 0;
#ifdef _WIN32
    shutdown(memdump_socket, SD_BOTH);
    closesocket(memdump_socket);
    WSACleanup();
#else
    shutdown(memdump_socket, SHUT_RDWR);
    close(memdump_socket);
#endif
    memdump_socket = -1;
}
