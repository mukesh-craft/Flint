#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
// v0.20 PORT: POSIX sockets vs Winsock2. MinGW provides the BSD headers, so
// this branch only triggers for MSVC-style toolchains (_WIN32 without the
// POSIX socket headers). Link Windows programs with -lws2_32.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <errno.h> // EINVAL/ENOMEM/etc. (socket errors use WSAGetLastError)
#else
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

// Socket compat layer: SOCKET handles are 64-bit on Win64 but int-like on// POSIX. The Flint API carries them as int64_t with -1 = invalid on both
// (INVALID_SOCKET is (SOCKET)~0, which reads back as -1).
#ifdef _WIN32
typedef SOCKET flint_sock_t;
#define FLINT_SOCK_INVALID INVALID_SOCKET
#define FLINT_SOCK_CLOSE closesocket
#define FLINT_SEND_FLAGS 0
#define FLINT_SOCK_ERR() WSAGetLastError()
#define FLINT_SOCK_EINTR WSAEINTR
static int flint_sock_init(void) {
    static LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    }
    return 0;
}
#else
typedef int flint_sock_t;
#define FLINT_SOCK_INVALID (-1)
#define FLINT_SOCK_CLOSE close
#define FLINT_SEND_FLAGS MSG_NOSIGNAL
#define FLINT_SOCK_ERR() errno
#define FLINT_SOCK_EINTR EINTR
static int flint_sock_init(void) { return 0; }
#endif

// MSVC's errno.h lacks some POSIX codes MinGW provides; map to Winsock's.
#ifdef _WIN32
#ifndef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#endif
#ifndef EPROTO
#define EPROTO WSAEPROTONOSUPPORT
#endif
#endif

extern void flint_panic(const char* msg);
extern int64_t flint_g_err;
extern void flint_set_err(int64_t err);

static void flint_safe_free(void* ptr)
{
    if (ptr) free(ptr);
}

static char* flint_strdup(const char* s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

int64_t flint_tcp_connect(const char* host, int64_t port)
{
    if (!host || port < 0 || port > 65535) {
        flint_set_err(EINVAL);
        return -1;
    }
    if (flint_sock_init() != 0) {
        flint_set_err(EIO); // init failure; EIO exists on every platform
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%" PRId64, port);

    struct addrinfo* result = NULL;
    int gai_err = getaddrinfo(host, port_str, &hints, &result);
    if (gai_err != 0 || !result) {
        flint_set_err(gai_err);
        return -1;
    }

    flint_sock_t fd = FLINT_SOCK_INVALID;
    struct addrinfo* rp;
    for (rp = result; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == FLINT_SOCK_INVALID) continue;

        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;

        FLINT_SOCK_CLOSE(fd);
        fd = FLINT_SOCK_INVALID;
    }

    freeaddrinfo(result);

    if (fd == FLINT_SOCK_INVALID) {
        int e = FLINT_SOCK_ERR();
        flint_set_err(e ? e : ECONNREFUSED);
        return -1;
    }

    return (int64_t)fd;
}

int64_t flint_tcp_listen(int64_t port)
{
    if (port < 0 || port > 65535) {
        flint_set_err(EINVAL);
        return -1;
    }
    if (flint_sock_init() != 0) {
        flint_set_err(EIO);
        return -1;
    }

    flint_sock_t fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd == FLINT_SOCK_INVALID) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == FLINT_SOCK_INVALID) {
            flint_set_err(FLINT_SOCK_ERR());
            return -1;
        }
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
        FLINT_SOCK_CLOSE(fd);
        flint_set_err(FLINT_SOCK_ERR());
        return -1;
    }

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons((uint16_t)port);
    addr6.sin6_addr = in6addr_any;

    if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((uint16_t)port);
        addr4.sin_addr.s_addr = INADDR_ANY;

        if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
            FLINT_SOCK_CLOSE(fd);
            flint_set_err(FLINT_SOCK_ERR());
            return -1;
        }
    }

    if (listen(fd, SOMAXCONN) < 0) {
        FLINT_SOCK_CLOSE(fd);
        flint_set_err(FLINT_SOCK_ERR());
        return -1;
    }

    return (int64_t)fd;
}

int64_t flint_tcp_accept(int64_t server_fd)
{
    flint_sock_t sfd = (flint_sock_t)server_fd;
    if (server_fd < 0) {
        flint_set_err(EINVAL);
        return -1;
    }

    struct sockaddr_storage addr;
#ifdef _WIN32
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif
    flint_sock_t client_fd = accept(sfd, (struct sockaddr*)&addr, &addrlen);
    if (client_fd == FLINT_SOCK_INVALID) {
        flint_set_err(FLINT_SOCK_ERR());
        return -1;
    }

    return (int64_t)client_fd;
}

int64_t flint_tcp_send(int64_t fd, const char* data, int64_t len)
{
    flint_sock_t sfd = (flint_sock_t)fd;
    if (fd < 0 || !data || len < 0) {
        flint_set_err(EINVAL);
        return -1;
    }

    int64_t total_sent = 0;
    while (total_sent < len) {
        const char* buf = data + total_sent;
        // Winsock takes int lengths; chunk for both APIs (also avoids
        // size_t->int truncation on huge sends).
        size_t remain = (size_t)(len - total_sent);
        if (remain > (size_t)INT_MAX) remain = (size_t)INT_MAX;
        int64_t n = (int64_t)send(sfd, buf, (int)remain, FLINT_SEND_FLAGS);
        if (n < 0) {
            int e = FLINT_SOCK_ERR();
            if (e == FLINT_SOCK_EINTR) continue;
            flint_set_err(e);
            return -1;
        }
        total_sent += n;
    }

    return total_sent;
}

char* flint_tcp_recv(int64_t fd, int64_t max_len)
{
    flint_sock_t sfd = (flint_sock_t)fd;
    if (fd < 0 || max_len < 0) {
        flint_set_err(EINVAL);
        return NULL;
    }
    if (max_len == 0) {
        char* buf = (char*)malloc(1);
        if (!buf) {
            flint_set_err(ENOMEM);
            return NULL;
        }
        buf[0] = '\0';
        return buf;
    }

    size_t cap = (size_t)max_len;
    char* buf = (char*)malloc(cap + 1);
    if (!buf) {
        flint_set_err(ENOMEM);
        return NULL;
    }

    int64_t n = (int64_t)recv(sfd, buf, cap > (size_t)INT_MAX ? INT_MAX : (int)cap, FLINT_SEND_FLAGS);
    if (n < 0) {
        flint_set_err(FLINT_SOCK_ERR());
        free(buf);
        return NULL;
    }

    buf[n] = '\0';
    return buf;
}

char* flint_tcp_recv_all(int64_t fd, int64_t max_len)
{
    int sfd = (int)fd;
    if (sfd < 0 || max_len < 0) {
        flint_set_err(EINVAL);
        return NULL;
    }
    if (max_len == 0) {
        char* buf = (char*)malloc(1);
        if (!buf) {
            flint_set_err(ENOMEM);
            return NULL;
        }
        buf[0] = '\0';
        return buf;
    }

    size_t cap = (size_t)max_len;
    char* buf = (char*)malloc(cap + 1);
    if (!buf) {
        flint_set_err(ENOMEM);
        return NULL;
    }

    int64_t total = 0;
    while (total < (int64_t)cap) {
        char* ptr = buf + total;
        size_t remain = cap - (size_t)total;
        if (remain > (size_t)INT_MAX) remain = (size_t)INT_MAX;
        int64_t n = (int64_t)recv(sfd, ptr, (int)remain, FLINT_SEND_FLAGS);
        if (n < 0) {
            int e = FLINT_SOCK_ERR();
            if (e == FLINT_SOCK_EINTR) continue;
            flint_set_err(e);
            free(buf);
            return NULL;
        }
        if (n == 0) break;
        total += n;
    }

    buf[total] = '\0';
    return buf;
}

void flint_tcp_close(int64_t fd)
{
    flint_sock_t sfd = (flint_sock_t)fd;
    if (fd >= 0 && sfd != FLINT_SOCK_INVALID) {
        FLINT_SOCK_CLOSE(sfd);
    }
}

int64_t flint_tcp_set_timeout(int64_t fd, int64_t timeout_ms)
{
    flint_sock_t sfd = (flint_sock_t)fd;
    if (fd < 0 || timeout_ms < 0) {
        flint_set_err(EINVAL);
        return -1;
    }

#ifdef _WIN32
    // Winsock takes milliseconds as a DWORD, not struct timeval.
    DWORD ms = timeout_ms > (int64_t)INFINITE ? INFINITE : (DWORD)timeout_ms;
    if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms)) < 0) {
        flint_set_err(FLINT_SOCK_ERR());
        return -1;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        flint_set_err(errno);
        return -1;
    }
#endif

    return 0;
}

char* flint_dns_resolve(const char* hostname)
{
    if (!hostname) {
        flint_set_err(EINVAL);
        return NULL;
    }
    // Winsock requires WSAStartup even for name resolution.
    if (flint_sock_init() != 0) {
        flint_set_err(EIO);
        return NULL;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    int gai_err = getaddrinfo(hostname, NULL, &hints, &result);
    if (gai_err != 0 || !result) {
        flint_set_err(gai_err);
        return NULL;
    }

    char ip[INET6_ADDRSTRLEN];
    ip[0] = '\0';

    struct addrinfo* rp;
    for (rp = result; rp; rp = rp->ai_next) {
        void* addr = NULL;
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)rp->ai_addr;
            addr = &sa->sin_addr;
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6* sa = (struct sockaddr_in6*)rp->ai_addr;
            addr = &sa->sin6_addr;
        } else {
            continue;
        }

        if (inet_ntop(rp->ai_family, addr, ip, sizeof(ip))) {
            break;
        }
    }

    freeaddrinfo(result);

    if (ip[0] == '\0') {
        flint_set_err(EAI_NONAME);
        return NULL;
    }

    return flint_strdup(ip);
}

typedef struct {
    char host[256];
    int64_t port;
    char path[1024];
} ParsedURL;

static bool parse_http_url(const char* url, ParsedURL* out)
{
    if (!url || !out) return false;

    memset(out, 0, sizeof(ParsedURL));
    out->port = 80;
    out->path[0] = '/';
    out->path[1] = '\0';

    const char* p = url;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        return false;
    }

    const char* host_start = p;

    while (*p && *p != '/' && *p != ':' && *p != '?') p++;
    const char* host_end = p;

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*p == ':') {
        p++;
        char* end = NULL;
        long port = strtol(p, &end, 10);
        if (end == p || port <= 0 || port > 65535) return false;
        out->port = (int64_t)port;
        p = end;
    }

    if (*p == '/') {
        size_t i = 0;
        while (*p && i < sizeof(out->path) - 1) {
            out->path[i++] = *p++;
        }
        out->path[i] = '\0';
    } else if (*p == '?') {
        out->path[0] = '/';
        size_t i = 1;
        while (*p && i < sizeof(out->path) - 1) {
            out->path[i++] = *p++;
        }
        out->path[i] = '\0';
    }

    return true;
}

static char* recv_http_response(int64_t fd, int64_t timeout_ms)
{
    if (timeout_ms > 0) {
        flint_tcp_set_timeout(fd, timeout_ms);
    }

    size_t cap = 4096;
    size_t len = 0;
    char* raw = (char*)malloc(cap);
    if (!raw) {
        flint_set_err(ENOMEM);
        return NULL;
    }

    flint_sock_t sock = (flint_sock_t)fd;
    while (true) {
        if (len + 1 >= cap) {
            cap *= 2;
            char* tmp = (char*)realloc(raw, cap);
            if (!tmp) {
                flint_set_err(ENOMEM);
                free(raw);
                return NULL;
            }
            raw = tmp;
        }

        size_t want = cap - len - 1;
        if (want > (size_t)INT_MAX) want = (size_t)INT_MAX;
        int64_t n = (int64_t)recv(sock, raw + len, (int)want, FLINT_SEND_FLAGS);
        if (n < 0) {
            int e = FLINT_SOCK_ERR();
            if (e == FLINT_SOCK_EINTR) continue;
            flint_set_err(e);
            free(raw);
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
    }

    raw[len] = '\0';

    const char* sep = strstr(raw, "\r\n\r\n");
    if (!sep) sep = strstr(raw, "\n\n");
    if (!sep) {
        free(raw);
        flint_set_err(EPROTO);
        return NULL;
    }

    const char* body_start;
    if (sep[0] == '\r') {
        body_start = sep + 4;
    } else {
        body_start = sep + 2;
    }

    size_t body_len = len - (size_t)(body_start - raw);
    char* body = (char*)malloc(body_len + 1);
    if (!body) {
        flint_set_err(ENOMEM);
        free(raw);
        return NULL;
    }

    memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    free(raw);
    return body;
}

char* flint_http_get(const char* url, int64_t timeout_ms)
{
    if (!url) {
        flint_set_err(EINVAL);
        return NULL;
    }

    ParsedURL purl;
    if (!parse_http_url(url, &purl)) {
        flint_set_err(EINVAL);
        return NULL;
    }

    int64_t fd = flint_tcp_connect(purl.host, purl.port);
    if (fd < 0) return NULL;

    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        purl.path, purl.host);

    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        flint_tcp_close(fd);
        flint_set_err(ENOSPC);
        return NULL;
    }

    int64_t sent = flint_tcp_send(fd, request, (int64_t)req_len);
    if (sent < 0) {
        flint_tcp_close(fd);
        return NULL;
    }

    char* body = recv_http_response(fd, timeout_ms);
    flint_tcp_close(fd);
    return body;
}

char* flint_http_post(const char* url, const char* content_type, const char* body, int64_t timeout_ms)
{
    if (!url || !content_type || !body) {
        flint_set_err(EINVAL);
        return NULL;
    }

    ParsedURL purl;
    if (!parse_http_url(url, &purl)) {
        flint_set_err(EINVAL);
        return NULL;
    }

    int64_t fd = flint_tcp_connect(purl.host, purl.port);
    if (fd < 0) return NULL;

    int64_t body_len = (int64_t)strlen(body);

    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %" PRId64 "\r\n"
        "Connection: close\r\n"
        "\r\n",
        purl.path, purl.host, content_type, body_len);

    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        flint_tcp_close(fd);
        flint_set_err(ENOSPC);
        return NULL;
    }

    if (flint_tcp_send(fd, request, (int64_t)req_len) < 0) {
        flint_tcp_close(fd);
        return NULL;
    }

    if (flint_tcp_send(fd, body, body_len) < 0) {
        flint_tcp_close(fd);
        return NULL;
    }

    char* resp = recv_http_response(fd, timeout_ms);
    flint_tcp_close(fd);
    return resp;
}
