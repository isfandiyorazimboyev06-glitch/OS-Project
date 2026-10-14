#include "common.h"

static int connect_to_server(const char *host, const char *port_str) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int s = getaddrinfo(host, port_str, &hints, &res);
    if (s != 0) {
        fprintf(stderr, "client: cannot connect to %s:%s\n", host, port_str);
        exit(3);
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; /* Success */
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stderr, "client: cannot connect to %s:%s\n", host, port_str);
        exit(3);
    }

    set_tcp_nodelay(sockfd);
    return sockfd;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server-host> <server-port> <verb> ...\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *port_str = argv[2];
    const char *verb = argv[3];

    int fd = connect_to_server(host, port_str);

    if (strcmp(verb, "submit") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: %s <host> <port> submit \"<command>\" <timeout> [priority]\n", argv[0]);
            close(fd);
            return 1;
        }
        const char *cmd = argv[4];
        int timeout = atoi(argv[5]);
        if (timeout < 1) timeout = 1;
        int priority = (argc >= 7) ? atoi(argv[6]) : 0;
        if (priority < 0) priority = 0;
        if (priority > 9) priority = 9;

        size_t cmd_len = strlen(cmd);
        char header[256];
        int hlen = snprintf(header, sizeof(header), "SUBMIT %d %d %zu\n", timeout, priority, cmd_len);
        if (write_all(fd, header, hlen) < 0 || write_all(fd, cmd, cmd_len) < 0) {
            close(fd);
            return 1;
        }

        char line[512];
        if (read_line(fd, line, sizeof(line)) <= 0) {
            close(fd);
            return 1;
        }

        close(fd);

        /* Server replies: JOB_ID <id> */
        char reply_type[32], job_id[128];
        if (sscanf(line, "%31s %127s", reply_type, job_id) == 2 && strcmp(reply_type, "JOB_ID") == 0) {
            printf("%s\n", job_id);
            return 0;
        } else {
            fprintf(stderr, "Unexpected server response: %s\n", line);
            return 1;
        }

    } else if (strcmp(verb, "status") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s <host> <port> status <id>\n", argv[0]);
            close(fd);
            return 1;
        }
        const char *job_id = argv[4];
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "STATUS %s\n", job_id);
        if (write_all(fd, msg, len) < 0) {
            close(fd);
            return 1;
        }

        char line[512];
        if (read_line(fd, line, sizeof(line)) <= 0) {
            close(fd);
            return 1;
        }
        close(fd);

        if (strcmp(line, "UNKNOWN") == 0) {
            printf("%s UNKNOWN\n", job_id);
            return 2;
        } else if (strncmp(line, "STATE ", 6) == 0) {
            printf("%s %s\n", job_id, line + 6);
            return 0;
        } else {
            fprintf(stderr, "Unexpected server response: %s\n", line);
            return 1;
        }

    } else if (strcmp(verb, "cancel") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s <host> <port> cancel <id>\n", argv[0]);
            close(fd);
            return 1;
        }
        const char *job_id = argv[4];
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "CANCEL %s\n", job_id);
        if (write_all(fd, msg, len) < 0) {
            close(fd);
            return 1;
        }

        char line[512];
        if (read_line(fd, line, sizeof(line)) <= 0) {
            close(fd);
            return 1;
        }
        close(fd);

        if (strcmp(line, "CANCEL_OK") == 0) {
            printf("%s CANCELLED\n", job_id);
            return 0;
        } else if (strcmp(line, "ALREADY_FINISHED") == 0) {
            printf("%s ALREADY FINISHED\n", job_id);
            return 1;
        } else if (strcmp(line, "UNKNOWN") == 0) {
            printf("%s UNKNOWN\n", job_id);
            return 2;
        } else {
            fprintf(stderr, "Unexpected server response: %s\n", line);
            return 1;
        }

    } else if (strcmp(verb, "fetch") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s <host> <port> fetch <id>\n", argv[0]);
            close(fd);
            return 1;
        }
        const char *job_id = argv[4];
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "FETCH %s\n", job_id);
        if (write_all(fd, msg, len) < 0) {
            close(fd);
            return 1;
        }

        char line[512];
        if (read_line(fd, line, sizeof(line)) <= 0) {
            close(fd);
            return 1;
        }

        if (strcmp(line, "UNKNOWN") == 0) {
            printf("%s UNKNOWN\n", job_id);
            close(fd);
            return 2;
        } else if (strcmp(line, "NOT_FINISHED") == 0) {
            fprintf(stderr, "%s NOT FINISHED\n", job_id);
            close(fd);
            return 1;
        } else if (strncmp(line, "FETCH_OK ", 9) == 0) {
            size_t output_len = (size_t)atoll(line + 9);
            size_t received = 0;
            char buf[4096];
            while (received < output_len) {
                size_t to_read = output_len - received;
                if (to_read > sizeof(buf)) to_read = sizeof(buf);
                ssize_t n = read(fd, buf, to_read);
                if (n <= 0) break;
                fwrite(buf, 1, n, stdout);
                received += n;
            }
            fflush(stdout);
            close(fd);
            return 0;
        } else {
            fprintf(stderr, "Unexpected server response: %s\n", line);
            close(fd);
            return 1;
        }
    } else {
        /* Any verb with an id never issued or invalid verb */
        /* If user called e.g. client <host> <port> foo <id> */
        const char *job_id = (argc >= 5) ? argv[4] : "UNKNOWN";
        printf("%s UNKNOWN\n", job_id);
        close(fd);
        return 2;
    }

    close(fd);
    return 0;
}
