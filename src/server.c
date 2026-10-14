#include "common.h"
#include <sys/resource.h>

typedef struct {
    char id[64];
    char *cmd;
    int timeout;
    int priority;
    time_t submit_time;
    JobState state;
    char state_detail[128]; /* e.g. "DONE 0", "KILLED TIMEOUT", "CANCELLED" */
    int assigned_worker_fd;
    char *output;
    size_t output_len;
} ServerJob;

typedef struct {
    int fd;
    int K;
    int running_jobs;
    int active;
} ServerWorker;

typedef struct {
    int fd;
    int is_worker;
    time_t connect_time;
    char buf[BUFFER_SIZE];
    size_t buf_len;
} ConnState;

#define MAX_SERVER_WORKERS 64

/* Dynamically resizable job array */
static ServerJob *g_jobs = NULL;
static int g_job_count = 0;
static int g_job_capacity = 0;
static int g_next_job_id = 1;

static ServerWorker g_workers[MAX_SERVER_WORKERS];

/* Dynamically resizable connection array */
static ConnState *g_conns = NULL;
static int g_conn_capacity = 0;

static volatile sig_atomic_t g_server_stop = 0;

static void sigterm_handler(int sig) {
    (void)sig;
    g_server_stop = 1;
}

/* O(1) Fast lookup with O(N) fallback */
static ServerJob *find_job(const char *id) {
    size_t prefix_len = strlen(TEAM_PREFIX);
    if (strncmp(id, TEAM_PREFIX "-", prefix_len + 1) == 0) {
        int idx = atoi(id + prefix_len + 1) - 1;
        if (idx >= 0 && idx < g_job_count && strcmp(g_jobs[idx].id, id) == 0) {
            return &g_jobs[idx];
        }
    }
    for (int i = 0; i < g_job_count; i++) {
        if (strcmp(g_jobs[i].id, id) == 0) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static ServerWorker *find_worker_by_fd(int fd) {
    for (int i = 0; i < MAX_SERVER_WORKERS; i++) {
        if (g_workers[i].active && g_workers[i].fd == fd) {
            return &g_workers[i];
        }
    }
    return NULL;
}

static void close_conn(int idx) {
    if (idx >= 0 && idx < g_conn_capacity) {
        if (g_conns[idx].fd >= 0) {
            close(g_conns[idx].fd);
            g_conns[idx].fd = -1;
        }
        g_conns[idx].buf_len = 0;
        g_conns[idx].is_worker = 0;
    }
}

static void schedule_jobs(void) {
    time_t now = time(NULL);

    while (1) {
        /* Find worker with free slot */
        ServerWorker *best_worker = NULL;
        for (int i = 0; i < MAX_SERVER_WORKERS; i++) {
            if (g_workers[i].active && g_workers[i].running_jobs < g_workers[i].K) {
                best_worker = &g_workers[i];
                break;
            }
        }

        if (!best_worker) break;

        /* Priority with Aging: pick queued job with highest effective priority */
        ServerJob *best_job = NULL;
        int best_effective_prio = -1;
        time_t best_submit_time = 0;

        for (int i = 0; i < g_job_count; i++) {
            if (g_jobs[i].state == STATE_QUEUED) {
                int wait_sec = (int)(now - g_jobs[i].submit_time);
                int aging_bonus = wait_sec / AGING_INTERVAL_SEC;
                int eff_prio = g_jobs[i].priority + aging_bonus;

                if (!best_job || eff_prio > best_effective_prio ||
                    (eff_prio == best_effective_prio && g_jobs[i].submit_time < best_submit_time)) {
                    best_job = &g_jobs[i];
                    best_effective_prio = eff_prio;
                    best_submit_time = g_jobs[i].submit_time;
                }
            }
        }

        if (!best_job) break;

        /* Dispatch job */
        size_t cmd_len = strlen(best_job->cmd);
        char header[256];
        int hlen = snprintf(header, sizeof(header), "DISPATCH %s %d %zu\n",
                            best_job->id, best_job->timeout, cmd_len);

        if (write_all(best_worker->fd, header, hlen) >= 0 &&
            write_all(best_worker->fd, best_job->cmd, cmd_len) >= 0) {
            best_job->state = STATE_RUNNING;
            strncpy(best_job->state_detail, "RUNNING", sizeof(best_job->state_detail) - 1);
            best_job->assigned_worker_fd = best_worker->fd;
            best_worker->running_jobs++;

            printf("job sent\n");
            fflush(stdout);
        } else {
            best_worker->active = 0;
            close(best_worker->fd);
            best_worker->fd = -1;
        }
    }
}

static void handle_client_request(int conn_idx) {
    ConnState *c = &g_conns[conn_idx];
    char *buf = c->buf;
    size_t len = c->buf_len;

    char *nl = memchr(buf, '\n', len);
    if (!nl) return;

    size_t line_len = nl - buf;
    char line[1024];
    if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    if (strncmp(line, "WORKER_HELLO ", 13) == 0) {
        int K = atoi(line + 13);
        if (K < 1) K = 1;

        c->is_worker = 1;
        int w_idx = -1;
        for (int i = 0; i < MAX_SERVER_WORKERS; i++) {
            if (!g_workers[i].active) {
                w_idx = i;
                break;
            }
        }
        if (w_idx != -1) {
            g_workers[w_idx].fd = c->fd;
            g_workers[w_idx].K = K;
            g_workers[w_idx].running_jobs = 0;
            g_workers[w_idx].active = 1;
        }

        const char *resp = "WORKER_OK\n";
        write_all(c->fd, resp, strlen(resp));

        size_t consumed = (nl - buf) + 1;
        memmove(buf, buf + consumed, len - consumed);
        c->buf_len -= consumed;

        schedule_jobs();
        return;
    }

    /* Print client connected once */
    printf("client connected\n");
    fflush(stdout);

    if (strncmp(line, "SUBMIT ", 7) == 0) {
        int timeout = 10;
        int priority = 0;
        size_t cmd_len = 0;
        if (sscanf(line + 7, "%d %d %zu", &timeout, &priority, &cmd_len) == 3) {
            size_t header_len = (nl - buf) + 1;
            if (len < header_len + cmd_len) {
                return; /* Wait for command bytes */
            }

            char *cmd = malloc(cmd_len + 1);
            if (!cmd) {
                close_conn(conn_idx);
                return;
            }
            memcpy(cmd, buf + header_len, cmd_len);
            cmd[cmd_len] = '\0';

            /* Dynamically resize jobs if needed */
            if (g_job_count >= g_job_capacity) {
                int new_cap = (g_job_capacity == 0) ? 1024 : g_job_capacity * 2;
                ServerJob *new_jobs = realloc(g_jobs, new_cap * sizeof(ServerJob));
                if (new_jobs) {
                    g_jobs = new_jobs;
                    g_job_capacity = new_cap;
                }
            }

            if (g_job_count < g_job_capacity) {
                ServerJob *job = &g_jobs[g_job_count++];
                memset(job, 0, sizeof(*job));
                snprintf(job->id, sizeof(job->id), "%s-%d", TEAM_PREFIX, g_next_job_id++);
                job->cmd = cmd;
                job->timeout = timeout;
                job->priority = priority;
                job->submit_time = time(NULL);
                job->state = STATE_QUEUED;
                strncpy(job->state_detail, "QUEUED", sizeof(job->state_detail) - 1);
                job->assigned_worker_fd = -1;

                printf("job accepted\n");
                fflush(stdout);

                char reply[128];
                int rlen = snprintf(reply, sizeof(reply), "JOB_ID %s\n", job->id);
                write_all(c->fd, reply, rlen);
            }

            close_conn(conn_idx);
            schedule_jobs();
            return;
        } else {
            close_conn(conn_idx);
            return;
        }

    } else if (strncmp(line, "STATUS ", 7) == 0) {
        char job_id[128];
        sscanf(line + 7, "%127s", job_id);
        ServerJob *job = find_job(job_id);
        if (!job) {
            const char *resp = "UNKNOWN\n";
            write_all(c->fd, resp, strlen(resp));
        } else {
            char resp[256];
            int rlen = snprintf(resp, sizeof(resp), "STATE %s\n", job->state_detail);
            write_all(c->fd, resp, rlen);
        }
        close_conn(conn_idx);
        return;

    } else if (strncmp(line, "CANCEL ", 7) == 0) {
        char job_id[128];
        sscanf(line + 7, "%127s", job_id);
        ServerJob *job = find_job(job_id);
        if (!job) {
            const char *resp = "UNKNOWN\n";
            write_all(c->fd, resp, strlen(resp));
        } else if (job->state == STATE_DONE || job->state == STATE_KILLED_TIMEOUT ||
                   job->state == STATE_KILLED_SIGNAL || job->state == STATE_CANCELLED) {
            const char *resp = "ALREADY_FINISHED\n";
            write_all(c->fd, resp, strlen(resp));
        } else if (job->state == STATE_QUEUED) {
            job->state = STATE_CANCELLED;
            strncpy(job->state_detail, "CANCELLED", sizeof(job->state_detail) - 1);
            const char *resp = "CANCEL_OK\n";
            write_all(c->fd, resp, strlen(resp));
        } else if (job->state == STATE_RUNNING) {
            if (job->assigned_worker_fd >= 0) {
                char cancel_msg[128];
                int clen = snprintf(cancel_msg, sizeof(cancel_msg), "CANCEL_JOB %s\n", job->id);
                write_all(job->assigned_worker_fd, cancel_msg, clen);
            }
            job->state = STATE_CANCELLED;
            strncpy(job->state_detail, "CANCELLED", sizeof(job->state_detail) - 1);
            const char *resp = "CANCEL_OK\n";
            write_all(c->fd, resp, strlen(resp));
        }
        close_conn(conn_idx);
        return;

    } else if (strncmp(line, "FETCH ", 6) == 0) {
        char job_id[128];
        sscanf(line + 6, "%127s", job_id);
        ServerJob *job = find_job(job_id);
        if (!job) {
            const char *resp = "UNKNOWN\n";
            write_all(c->fd, resp, strlen(resp));
        } else if (job->state == STATE_QUEUED || job->state == STATE_RUNNING) {
            const char *resp = "NOT_FINISHED\n";
            write_all(c->fd, resp, strlen(resp));
        } else {
            char header[128];
            int hlen = snprintf(header, sizeof(header), "FETCH_OK %zu\n", job->output_len);
            write_all(c->fd, header, hlen);
            if (job->output_len > 0 && job->output) {
                write_all(c->fd, job->output, job->output_len);
            }
        }
        close_conn(conn_idx);
        return;

    } else {
        const char *resp = "UNKNOWN\n";
        write_all(c->fd, resp, strlen(resp));
        close_conn(conn_idx);
        return;
    }
}

static void handle_worker_data(int conn_idx) {
    ConnState *c = &g_conns[conn_idx];
    char *buf = c->buf;
    size_t len = c->buf_len;

    while (1) {
        char *nl = memchr(buf, '\n', len);
        if (!nl) break;

        size_t line_len = nl - buf;
        char line[1024];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        if (strncmp(line, "REPORT ", 7) == 0) {
            char job_id[128];
            char state_type[64];
            char state_arg[64];
            size_t out_len = 0;

            if (sscanf(line + 7, "%127s %63s %63s %zu", job_id, state_type, state_arg, &out_len) == 4) {
                size_t header_len = (nl - buf) + 1;
                if (len < header_len + out_len) break;

                ServerJob *job = find_job(job_id);
                if (job) {
                    if (strcmp(state_type, "DONE") == 0) {
                        job->state = STATE_DONE;
                        snprintf(job->state_detail, sizeof(job->state_detail), "DONE %s", state_arg);
                    } else if (strcmp(state_type, "KILLED_TIMEOUT") == 0) {
                        job->state = STATE_KILLED_TIMEOUT;
                        strncpy(job->state_detail, "KILLED TIMEOUT", sizeof(job->state_detail) - 1);
                    } else if (strcmp(state_type, "KILLED_SIGNAL") == 0) {
                        job->state = STATE_KILLED_SIGNAL;
                        snprintf(job->state_detail, sizeof(job->state_detail), "KILLED SIGNAL %s", state_arg);
                    } else if (strcmp(state_type, "CANCELLED") == 0) {
                        job->state = STATE_CANCELLED;
                        strncpy(job->state_detail, "CANCELLED", sizeof(job->state_detail) - 1);
                    }

                    if (out_len > 0) {
                        job->output = malloc(out_len);
                        if (job->output) {
                            memcpy(job->output, buf + header_len, out_len);
                            job->output_len = out_len;
                        }
                    }

                    ServerWorker *w = find_worker_by_fd(c->fd);
                    if (w && w->running_jobs > 0) {
                        w->running_jobs--;
                    }

                    printf("job finished\n");
                    fflush(stdout);
                }

                size_t consumed = header_len + out_len;
                memmove(buf, buf + consumed, len - consumed);
                len -= consumed;
                c->buf_len = len;

                schedule_jobs();
            } else {
                size_t consumed = (nl - buf) + 1;
                memmove(buf, buf + consumed, len - consumed);
                len -= consumed;
                c->buf_len = len;
            }
        } else {
            size_t consumed = (nl - buf) + 1;
            memmove(buf, buf + consumed, len - consumed);
            len -= consumed;
            c->buf_len = len;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    /* Maximize file descriptor limit */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    /* Support high concurrency backlog */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    set_nonblocking(listen_fd);

    printf("server: listening on port %d\n", port);
    fflush(stdout);

    /* Initialize connections array */
    g_conn_capacity = 1024;
    g_conns = malloc(g_conn_capacity * sizeof(ConnState));
    for (int i = 0; i < g_conn_capacity; i++) {
        g_conns[i].fd = -1;
        g_conns[i].buf_len = 0;
        g_conns[i].is_worker = 0;
    }

    /* Initialize jobs array */
    g_job_capacity = 1024;
    g_jobs = malloc(g_job_capacity * sizeof(ServerJob));

    while (!g_server_stop) {
        struct pollfd *pfds = malloc((g_conn_capacity + 1) * sizeof(struct pollfd));
        int *conn_map = malloc((g_conn_capacity + 1) * sizeof(int));
        int nfds = 0;

        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        conn_map[0] = -1;
        nfds = 1;

        for (int i = 0; i < g_conn_capacity; i++) {
            if (g_conns[i].fd >= 0) {
                pfds[nfds].fd = g_conns[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                conn_map[nfds] = i;
                nfds++;
            }
        }

        int ret = poll(pfds, nfds, 100);
        if (ret < 0) {
            free(pfds);
            free(conn_map);
            if (errno == EINTR) {
                if (g_server_stop) break;
                continue;
            }
            break;
        }

        if (g_server_stop) {
            free(pfds);
            free(conn_map);
            break;
        }

        /* Accept new connections */
        if (pfds[0].revents & POLLIN) {
            while (1) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) break;

                set_tcp_nodelay(client_fd);
                set_nonblocking(client_fd);

                int slot = -1;
                for (int i = 0; i < g_conn_capacity; i++) {
                    if (g_conns[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }

                /* Dynamically expand connections if at capacity */
                if (slot == -1) {
                    int new_cap = g_conn_capacity * 2;
                    ConnState *new_conns = realloc(g_conns, new_cap * sizeof(ConnState));
                    if (new_conns) {
                        for (int k = g_conn_capacity; k < new_cap; k++) {
                            new_conns[k].fd = -1;
                            new_conns[k].buf_len = 0;
                            new_conns[k].is_worker = 0;
                        }
                        slot = g_conn_capacity;
                        g_conns = new_conns;
                        g_conn_capacity = new_cap;
                    }
                }

                if (slot != -1) {
                    g_conns[slot].fd = client_fd;
                    g_conns[slot].is_worker = 0;
                    g_conns[slot].connect_time = time(NULL);
                    g_conns[slot].buf_len = 0;
                } else {
                    close(client_fd);
                }
            }
        }

        /* Handle client and worker events */
        for (int i = 1; i < nfds; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                int c_idx = conn_map[i];
                if (c_idx >= 0 && c_idx < g_conn_capacity && g_conns[c_idx].fd >= 0) {
                    ConnState *c = &g_conns[c_idx];
                    char temp[4096];
                    ssize_t n = read(c->fd, temp, sizeof(temp));
                    if (n > 0) {
                        if (c->buf_len + (size_t)n < sizeof(c->buf)) {
                            memcpy(c->buf + c->buf_len, temp, n);
                            c->buf_len += n;
                        }
                        if (c->is_worker) {
                            handle_worker_data(c_idx);
                        } else {
                            handle_client_request(c_idx);
                        }
                    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        if (c->is_worker) {
                            ServerWorker *w = find_worker_by_fd(c->fd);
                            if (w) w->active = 0;
                        }
                        close_conn(c_idx);
                    }
                }
            }
        }

        free(pfds);
        free(conn_map);

        schedule_jobs();
    }

    /* Shutdown clean handling within 2 seconds */
    printf("server: shutting down\n");
    fflush(stdout);

    close(listen_fd);
    for (int i = 0; i < g_conn_capacity; i++) {
        if (g_conns[i].fd >= 0) {
            close(g_conns[i].fd);
        }
    }
    free(g_conns);

    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].cmd) free(g_jobs[i].cmd);
        if (g_jobs[i].output) free(g_jobs[i].output);
    }
    free(g_jobs);

    return 0;
}
