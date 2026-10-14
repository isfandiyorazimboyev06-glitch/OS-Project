#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include "protocol.h"

/* Robust write to file descriptor */
static inline ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *ptr = (const char *)buf;
    while (total < count) {
        ssize_t n = write(fd, ptr + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    return (ssize_t)total;
}

/* Robust read exact count of bytes */
static inline ssize_t read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *ptr = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, ptr + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    return (ssize_t)total;
}

/* Read a single line up to \n (consuming \n). Null-terminates buf. Returns length or -1. */
static inline ssize_t read_line(int fd, char *buf, size_t maxlen) {
    size_t idx = 0;
    while (idx + 1 < maxlen) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (idx == 0) return 0; /* EOF */
            break;
        }
        if (c == '\n') {
            break;
        }
        buf[idx++] = c;
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

/* Set socket to non-blocking */
static inline int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Set TCP_NODELAY */
static inline int set_tcp_nodelay(int fd) {
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

#endif /* COMMON_H */
