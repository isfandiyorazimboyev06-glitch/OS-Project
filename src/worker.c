#include "common.h"
#include <dirent.h>

#define MAX_WORKER_JOBS 64

enum KillReason {
    KILL_NONE = 0,
    KILL_TIMEOUT,
    KILL_CANCEL,
    KILL_SIGTERM
};

typedef struct {
    int active;
    char id[128];
    pid_t pid;
    int pipe_fd;
    int timeout_sec;
    time_t start_time;
    int kill_reason;
    char *output_buf;
    size_t output_len;
    size_t output_cap;
    int child_exited;
    int exit_status;
    int pipe_eof;
} WorkerJob;

static WorkerJob g_jobs[MAX_WORKER_JOBS];
static volatile sig_atomic_t g_sigterm = 0;
static int g_server_fd = -1;
static int g_K = DEFAULT_K;

static void sigterm_handler(int sig) {
    (void)sig;
    g_sigterm = 1;
}

static void close_all_inherited_fds(int except_fd) {
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        int dfd = dirfd(dir);
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            int fd = atoi(de->d_name);
            if (fd > 2 && fd != except_fd && fd != dfd) {
                close(fd);
            }
        }
        closedir(dir);
    } else {
        /* Fallback */
        for (int fd = 3; fd < 1024; fd++) {
            if (fd != except_fd) {
                close(fd);
            }
        }
    }
}

static WorkerJob *find_job_by_id(const char *id) {
    for (int i = 0; i < MAX_WORKER_JOBS; i++) {
        if (g_jobs[i].active && strcmp(g_jobs[i].id, id) == 0) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static WorkerJob *find_job_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_WORKER_JOBS; i++) {
        if (g_jobs[i].active && g_jobs[i].pid == pid) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static void drain_job_pipe(WorkerJob *job) {
    if (job->pipe_fd < 0) return;
    char buf[4096];
    while (1) {
        ssize_t n = read(job->pipe_fd, buf, sizeof(buf));
        if (n > 0) {
            if (job->output_len + (size_t)n <= MAX_OUTPUT_SIZE) {
                if (job->output_len + (size_t)n > job->output_cap) {
                    size_t new_cap = job->output_cap * 2;
                    while (new_cap < job->output_len + (size_t)n) new_cap *= 2;
                    if (new_cap > MAX_OUTPUT_SIZE) new_cap = MAX_OUTPUT_SIZE;
                    char *new_buf = realloc(job->output_buf, new_cap);
                    if (new_buf) {
                        job->output_buf = new_buf;
                        job->output_cap = new_cap;
                    }
                }
                size_t to_copy = (size_t)n;
                if (job->output_len + to_copy > job->output_cap) {
                    to_copy = job->output_cap - job->output_len;
                }
                memcpy(job->output_buf + job->output_len, buf, to_copy);
                job->output_len += to_copy;
            }
        } else if (n == 0) {
            /* EOF reached */
            close(job->pipe_fd);
            job->pipe_fd = -1;
            job->pipe_eof = 1;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            /* Error */
            close(job->pipe_fd);
            job->pipe_fd = -1;
            job->pipe_eof = 1;
            break;
        }
    }
}

static void report_job_completion(WorkerJob *job) {
    /* Final drain of pipe */
    drain_job_pipe(job);
    if (job->pipe_fd >= 0) {
        close(job->pipe_fd);
        job->pipe_fd = -1;
    }

    char state_str[64];
    if (job->kill_reason == KILL_TIMEOUT) {
        snprintf(state_str, sizeof(state_str), "KILLED_TIMEOUT 0");
    } else if (job->kill_reason == KILL_CANCEL) {
        snprintf(state_str, sizeof(state_str), "CANCELLED 0");
    } else if (job->kill_reason == KILL_SIGTERM) {
        snprintf(state_str, sizeof(state_str), "KILLED_SIGNAL 9");
    } else if (WIFEXITED(job->exit_status)) {
        snprintf(state_str, sizeof(state_str), "DONE %d", WEXITSTATUS(job->exit_status));
    } else if (WIFSIGNALED(job->exit_status)) {
        snprintf(state_str, sizeof(state_str), "KILLED_SIGNAL %d", WTERMSIG(job->exit_status));
    } else {
        snprintf(state_str, sizeof(state_str), "DONE 0");
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header), "REPORT %s %s %zu\n",
                        job->id, state_str, job->output_len);
    if (write_all(g_server_fd, header, hlen) >= 0) {
        if (job->output_len > 0) {
            write_all(g_server_fd, job->output_buf, job->output_len);
        }
    }

    free(job->output_buf);
    job->output_buf = NULL;
    job->active = 0;
}

static void handle_sigterm_shutdown(void) {
    /* Kill all running jobs, reap them, report them as KILLED SIGNAL 9, and exit */
    for (int i = 0; i < MAX_WORKER_JOBS; i++) {
        if (g_jobs[i].active) {
            WorkerJob *job = &g_jobs[i];
            job->kill_reason = KILL_SIGTERM;
            if (!job->child_exited) {
                kill(-job->pid, SIGKILL);
                int status;
                waitpid(job->pid, &status, 0);
                job->child_exited = 1;
                job->exit_status = status;
            }
            report_job_completion(job);
        }
    }
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
    exit(0);
}

static void start_job(const char *id, int timeout, const char *cmd) {
    int slot = -1;
    for (int i = 0; i < MAX_WORKER_JOBS; i++) {
        if (!g_jobs[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        fprintf(stderr, "worker: no free job slot\n");
        return;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        return;
    }

    if (pid == 0) {
        /* Child process */
        /* Make child group leader so kill(-pid, SIGKILL) kills whole tree */
        setpgid(0, 0);

        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        if (pfd[1] != STDOUT_FILENO && pfd[1] != STDERR_FILENO) {
            close(pfd[1]);
        }

        /* Close all other inherited file descriptors */
        close_all_inherited_fds(-1);

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent process */
    close(pfd[1]); /* Close write end immediately */
    set_nonblocking(pfd[0]);

    WorkerJob *job = &g_jobs[slot];
    memset(job, 0, sizeof(*job));
    job->active = 1;
    snprintf(job->id, sizeof(job->id), "%s", id);
    job->pid = pid;
    job->pipe_fd = pfd[0];
    job->timeout_sec = timeout;
    job->start_time = time(NULL);
    job->kill_reason = KILL_NONE;
    job->output_cap = 65536;
    job->output_len = 0;
    job->output_buf = malloc(job->output_cap);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server-host> <server-port> [K]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    if (argc >= 4) {
        g_K = atoi(argv[3]);
        if (g_K < 1) g_K = 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Connect to server */
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        perror("getaddrinfo");
        return 1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        g_server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (g_server_fd == -1) continue;
        if (connect(g_server_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }
        close(g_server_fd);
        g_server_fd = -1;
    }
    freeaddrinfo(res);

    if (g_server_fd == -1) {
        fprintf(stderr, "worker: cannot connect to %s:%d\n", host, port);
        return 1;
    }

    set_tcp_nodelay(g_server_fd);

    /* Send WORKER_HELLO */
    char hello[128];
    int hlen = snprintf(hello, sizeof(hello), "WORKER_HELLO %d\n", g_K);
    if (write_all(g_server_fd, hello, hlen) < 0) {
        perror("write hello");
        close(g_server_fd);
        return 1;
    }

    char reply[128];
    if (read_line(g_server_fd, reply, sizeof(reply)) <= 0 || strcmp(reply, "WORKER_OK") != 0) {
        fprintf(stderr, "worker: handshake failed (%s)\n", reply);
        close(g_server_fd);
        return 1;
    }

    /* Print required startup line */
    printf("worker: connected to %s:%d, K=%d\n", host, port, g_K);
    fflush(stdout);

    /* Main worker loop */
    char srv_buf[BUFFER_SIZE];
    size_t srv_buf_len = 0;

    while (!g_sigterm) {
        /* Check timeouts for all active jobs */
        time_t now = time(NULL);
        for (int i = 0; i < MAX_WORKER_JOBS; i++) {
            if (g_jobs[i].active && !g_jobs[i].child_exited && g_jobs[i].kill_reason == KILL_NONE) {
                if (now - g_jobs[i].start_time >= g_jobs[i].timeout_sec) {
                    g_jobs[i].kill_reason = KILL_TIMEOUT;
                    kill(-g_jobs[i].pid, SIGKILL);
                }
            }
        }

        /* Check for reaped children */
        int status;
        pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            WorkerJob *job = find_job_by_pid(p);
            if (job) {
                job->child_exited = 1;
                job->exit_status = status;
            }
        }

        /* Check which jobs are completely done (child exited and pipe EOF) */
        for (int i = 0; i < MAX_WORKER_JOBS; i++) {
            if (g_jobs[i].active) {
                drain_job_pipe(&g_jobs[i]);
                if (g_jobs[i].child_exited && (g_jobs[i].pipe_eof || g_jobs[i].pipe_fd < 0)) {
                    report_job_completion(&g_jobs[i]);
                }
            }
        }

        /* Build pollfd array */
        struct pollfd pfds[MAX_WORKER_JOBS + 1];
        int nfds = 0;

        pfds[nfds].fd = g_server_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        int job_idx_map[MAX_WORKER_JOBS + 1];
        job_idx_map[0] = -1;

        for (int i = 0; i < MAX_WORKER_JOBS; i++) {
            if (g_jobs[i].active && g_jobs[i].pipe_fd >= 0) {
                pfds[nfds].fd = g_jobs[i].pipe_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                job_idx_map[nfds] = i;
                nfds++;
            }
        }

        int ret = poll(pfds, nfds, 100); /* 100 ms tick */
        if (ret < 0) {
            if (errno == EINTR) {
                if (g_sigterm) break;
                continue;
            }
            break;
        }

        if (g_sigterm) break;

        /* Check pipe data */
        for (int i = 1; i < nfds; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                int j_idx = job_idx_map[i];
                if (j_idx >= 0 && g_jobs[j_idx].active) {
                    drain_job_pipe(&g_jobs[j_idx]);
                }
            }
        }

        /* Check server commands */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char temp[2048];
            ssize_t n = read(g_server_fd, temp, sizeof(temp));
            if (n <= 0) {
                /* Server disconnected */
                break;
            }
            if (srv_buf_len + (size_t)n < sizeof(srv_buf)) {
                memcpy(srv_buf + srv_buf_len, temp, n);
                srv_buf_len += n;
            }

            /* Process complete lines/messages in srv_buf */
            while (1) {
                char *newline = memchr(srv_buf, '\n', srv_buf_len);
                if (!newline) break;

                size_t line_len = newline - srv_buf;
                char line[1024];
                if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
                memcpy(line, srv_buf, line_len);
                line[line_len] = '\0';

                if (strncmp(line, "CANCEL_JOB ", 11) == 0) {
                    char target_id[128];
                    sscanf(line + 11, "%127s", target_id);
                    WorkerJob *job = find_job_by_id(target_id);
                    if (job && !job->child_exited) {
                        job->kill_reason = KILL_CANCEL;
                        kill(-job->pid, SIGKILL);
                        /* Reap child immediately */
                        int st;
                        waitpid(job->pid, &st, 0);
                        job->child_exited = 1;
                        job->exit_status = st;
                        drain_job_pipe(job);
                        report_job_completion(job);
                    }
                    /* Remove line from srv_buf */
                    size_t consumed = (newline - srv_buf) + 1;
                    memmove(srv_buf, srv_buf + consumed, srv_buf_len - consumed);
                    srv_buf_len -= consumed;

                } else if (strncmp(line, "DISPATCH ", 9) == 0) {
                    char job_id[128];
                    int timeout = 10;
                    size_t cmd_len = 0;
                    if (sscanf(line + 9, "%127s %d %zu", job_id, &timeout, &cmd_len) == 3) {
                        size_t header_len = (newline - srv_buf) + 1;
                        if (srv_buf_len < header_len + cmd_len) {
                            /* Need to wait for full command bytes */
                            break;
                        }
                        char *cmd = malloc(cmd_len + 1);
                        if (cmd) {
                            memcpy(cmd, srv_buf + header_len, cmd_len);
                            cmd[cmd_len] = '\0';
                            start_job(job_id, timeout, cmd);
                            free(cmd);
                        }
                        size_t consumed = header_len + cmd_len;
                        memmove(srv_buf, srv_buf + consumed, srv_buf_len - consumed);
                        srv_buf_len -= consumed;
                    } else {
                        /* Malformed, drop line */
                        size_t consumed = (newline - srv_buf) + 1;
                        memmove(srv_buf, srv_buf + consumed, srv_buf_len - consumed);
                        srv_buf_len -= consumed;
                    }
                } else {
                    /* Unrecognized message, skip line */
                    size_t consumed = (newline - srv_buf) + 1;
                    memmove(srv_buf, srv_buf + consumed, srv_buf_len - consumed);
                    srv_buf_len -= consumed;
                }
            }
        }
    }

    if (g_sigterm) {
        handle_sigterm_shutdown();
    }

    /* Clean exit */
    for (int i = 0; i < MAX_WORKER_JOBS; i++) {
        if (g_jobs[i].active) {
            if (g_jobs[i].pipe_fd >= 0) close(g_jobs[i].pipe_fd);
            if (!g_jobs[i].child_exited) {
                kill(-g_jobs[i].pid, SIGKILL);
                waitpid(g_jobs[i].pid, NULL, 0);
            }
            if (g_jobs[i].output_buf) free(g_jobs[i].output_buf);
        }
    }
    if (g_server_fd >= 0) close(g_server_fd);
    return 0;
}
