/*
 * grep, as installed at /usr/bin/grep: hands this invocation to grepd.
 *
 * Sends grepd its arguments, the environment variables grep reads, its
 * standard streams and its working directory (the descriptors themselves,
 * over a unix socket), then waits for grep's exit and ends the same way:
 * the same status, or the same signal.
 *
 * Wire format, all integers native-endian uint32:
 *   header:  payload length, fd mask (bit n: stdio fd n is open)
 *            + SCM_RIGHTS: the open stdio fds in order, then the cwd
 *   payload: argc, then (length, bytes) per argument; envc, then the same
 *            per NAME=value
 *   reply:   kind (0 exited, 1 killed by a signal, 2 grepd could not run
 *            grep), value (the status or signal)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/grepd/grepd.sock"

extern char **environ;

/* grepd enforces its own list; this one only keeps the message small. */
static int grep_reads(const char *entry) {
    static const char *names[] = {
        "LANG=", "LANGUAGE=", "LC_", "GREP_COLOR=", "GREP_COLORS=", "TERM=",
        "POSIXLY_CORRECT=", NULL,
    };
    for (const char **name = names; *name; name++)
        if (strncmp(entry, *name, strlen(*name)) == 0)
            return 1;
    return 0;
}

struct buffer {
    char *data;
    size_t len, cap;
};

static void append(struct buffer *b, const void *data, size_t len) {
    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2;
        b->data = realloc(b->data, b->cap);
        if (!b->data) {
            perror("grep");
            exit(2);
        }
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void append_u32(struct buffer *b, uint32_t value) { append(b, &value, sizeof value); }

static void append_string(struct buffer *b, const char *s) {
    append_u32(b, (uint32_t)strlen(s));
    append(b, s, strlen(s));
}

static int write_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, char *data, size_t len) {
    while (len) {
        ssize_t n = read(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int fail(const char *prog, const char *what) {
    fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
    return 2;
}

int main(int argc, char **argv) {
    const char *prog = argc > 0 ? argv[0] : "grep";

    struct buffer payload = {0};
    append_u32(&payload, (uint32_t)argc);
    for (int i = 0; i < argc; i++)
        append_string(&payload, argv[i]);
    uint32_t envc = 0;
    for (char **e = environ; *e; e++)
        envc += grep_reads(*e);
    append_u32(&payload, envc);
    for (char **e = environ; *e; e++)
        if (grep_reads(*e))
            append_string(&payload, *e);

    /* O_PATH: a directory we may search but not list is still a working directory. */
    int cwd = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cwd < 0)
        return fail(prog, "cannot open the working directory");
    int fds[4], nfds = 0;
    uint32_t mask = 0;
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) != -1) {
            fds[nfds++] = fd;
            mask |= 1u << fd;
        }
    fds[nfds++] = cwd;

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof addr.sun_path - 1);
    if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0)
        return fail(prog, "cannot reach grepd at " SOCKET_PATH);

    uint32_t header[2] = {(uint32_t)payload.len, mask};
    struct iovec iov = {.iov_base = header, .iov_len = sizeof header};
    union {
        char buf[CMSG_SPACE(sizeof fds)];
        struct cmsghdr align;
    } control = {0};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.buf, .msg_controllen = CMSG_SPACE(nfds * sizeof(int)),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(nfds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));
    ssize_t sent;
    do
        sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
    while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)sizeof header || write_all(sock, payload.data, payload.len) < 0)
        return fail(prog, "cannot send to grepd");
    close(cwd);

    uint32_t reply[2];
    if (read_all(sock, (char *)reply, sizeof reply) < 0) {
        fprintf(stderr, "%s: grepd closed the connection\n", prog);
        return 2;
    }
    if (reply[0] == 0)
        return (int)reply[1];
    if (reply[0] == 1) {
        /* Die the way grep did, so a shell sees the same status (e.g. SIGPIPE under head). */
        int sig = (int)reply[1];
        signal(sig, SIG_DFL);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, sig);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        raise(sig);
        return 128 + sig;
    }
    fprintf(stderr, "%s: grepd could not run grep\n", prog);
    return 2;
}
