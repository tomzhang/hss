#include <sys/wait.h>
#include "common.h"
#include "sstring.h"
#include "executor.h"
#include "slot.h"
#include "command.h"

#define MAXFD 1024

#define BLOCK_SIGCHLD                           \
    sigemptyset(&sigmask);                      \
    sigaddset(&sigmask, SIGCHLD);               \
    sigprocmask(SIG_BLOCK, &sigmask, &osigmask)
#define UNBLOCK_SIGCHLD                         \
    sigprocmask(SIG_SETMASK, &osigmask, NULL)

sigset_t sigmask;
sigset_t osigmask;

volatile int alive_children = 0;

static void
print_line(struct slot *pslot, int io_type, sstring buf, void *data) {
    printf(ANSI_COLOR_GREEN "%s -> " ANSI_COLOR_RESET "%s", pslot->host, buf);
    fflush(stdout);
}

static void
save_string(struct slot *pslot, int io_type, sstring buf, void *data) {
    sstring str;
    if (!data) {
        return;
    }
    str = *((sstring *) data);
    str = string_append(str, buf, string_length(buf));
    *(sstring *) data = str;
}

static void
exec_ssh_cmd(struct slot *pslot, char *cmd) {
    char timeout_argv[64];
    char *ssh_argv[128];
    int idx = 0;
    int i;
    int ret;

    close(STDIN_FILENO);

    // printf("cmd: %s\n", cmd);

    close(pslot->io.out[PIPE_READ_END]);
    close(pslot->io.err[PIPE_READ_END]);

    if (dup2(pslot->io.out[PIPE_WRITE_END], STDOUT_FILENO) == -1) {
        eprintf("failed to dup stdout: %s\n", strerror(errno));
    }
    if (dup2(pslot->io.err[PIPE_WRITE_END], STDERR_FILENO) == -1) {
        eprintf("failed to dup stderr: %s\n", strerror(errno));
    }

    snprintf(timeout_argv, sizeof timeout_argv, "-oConnectTimeout=%d", pconfig->conn_timeout);

    ssh_argv[idx++] = "-q";
    ssh_argv[idx++] = "-oNumberOfPasswordPrompts=0";
    ssh_argv[idx++] = "-oStrictHostKeyChecking=no";
    ssh_argv[idx++] = timeout_argv;
    for (i = 0; i < pslot->ssh_argc; ++i) {
        ssh_argv[idx++] = pslot->ssh_argv[i];
    }
    ssh_argv[idx++] = cmd;
    ssh_argv[idx++] = NULL;

    ret = execvp("ssh", ssh_argv);

    eprintf("failed to exec the ssh binary: (%d) %s\n", ret, strerror(errno));
    exit(1);
}

static struct slot *
fork_ssh(struct slot *pslot, char *cmd) {
    int pid;
    slot_reinit(pslot);
    switch (pid = fork()) {
        case 0:
            // child
            exec_ssh_cmd(pslot, cmd);
            break;
        case -1:
            // error
            eprintf("unable to fork: %s\n", strerror(errno));
            return NULL;
        default:
            pslot->pid = pid;
            pslot->alive = true;
            close(pslot->io.out[PIPE_WRITE_END]);
            close(pslot->io.err[PIPE_WRITE_END]);
            alive_children++;
            if (pconfig->verbose) {
                printf("pid [%d] run in main thread, host: %s\n", pslot->pid, pslot->host);
            }
            break;
    }
    return pslot;
}

static void
fdset_alive_slots(struct slot *pslot, fd_set *readfds) {
    while (pslot) {
        if (!pslot->alive) {
            pslot = pslot->next;
            continue;
        }
        FD_SET(pslot->io.out[PIPE_READ_END], readfds);
        FD_SET(pslot->io.err[PIPE_READ_END], readfds);
        pslot = pslot->next;
    }
}

static void
read_alive_slots(struct slot *pslot, fd_set *readfds) {
    while (pslot) {
        if (!pslot->alive) {
            pslot = pslot->next;
            continue;
        }
        if (FD_ISSET(pslot->io.out[PIPE_READ_END], readfds)) {
            slot_read_line(pslot, STDOUT_FILENO, print_line, NULL);
        }
        if (FD_ISSET(pslot->io.err[PIPE_READ_END], readfds)) {
            slot_read_line(pslot, STDERR_FILENO, print_line, NULL);
        }
        pslot = pslot->next;
    }
}

static void
read_dead_slots(struct slot *pslot, struct slot *pslot_end) {
    while (pslot) {
        if (!pslot->alive) {
            slot_read_remains(pslot, STDOUT_FILENO, print_line, NULL);
            slot_read_remains(pslot, STDERR_FILENO, print_line, NULL);
        }
        pslot = pslot->next;
    }
}

int
exec_remote_cmd(struct slot *pslot, char *cmd) {
    fd_set readfds;
    struct slot *pslot_head = pslot;
    struct timeval no_timeout;
    struct timeval *timeout;

    memset(&no_timeout, 0, sizeof(struct timeval));

    alive_children = 0;

    while (pslot || alive_children) {
        BLOCK_SIGCHLD;
        if (pslot) {
            fork_ssh(pslot, cmd);
            pslot = pslot->next;
            usleep(100 * 1000);
        }

        read_dead_slots(pslot_head, pslot);

        FD_ZERO(&readfds);
        fdset_alive_slots(pslot_head, &readfds);

        if (!pslot) {
            timeout = NULL;
        } else {
            timeout = &no_timeout;
        }

        if (select(MAXFD, &readfds, NULL, NULL, timeout) > 0) {
            read_alive_slots(pslot_head, &readfds);
        }
        UNBLOCK_SIGCHLD;
    }

    read_dead_slots(pslot_head, pslot);

    return 0;
}

int
sync_exec_remote_cmd(struct slot *pslot, char *cmd, sstring *out, sstring *err) {

    fd_set readfds;

    if (!pslot) {
        return 0;
    }

    FD_ZERO(&readfds);

    BLOCK_SIGCHLD;
    fork_ssh(pslot, cmd);
    FD_SET(pslot->io.out[PIPE_READ_END], &readfds);
    FD_SET(pslot->io.err[PIPE_READ_END], &readfds);
    UNBLOCK_SIGCHLD;

    while (alive_children) {
        BLOCK_SIGCHLD;
        if (select(MAXFD, &readfds, NULL, NULL, NULL) > 0) {
            if (FD_ISSET(pslot->io.out[PIPE_READ_END], &readfds)) {
                slot_read_line(pslot, STDOUT_FILENO, save_string, out);
            }
            if (FD_ISSET(pslot->io.err[PIPE_READ_END], &readfds)) {
                slot_read_line(pslot, STDERR_FILENO, save_string, err);
            }
        }
        UNBLOCK_SIGCHLD;
    }
    return 0;
}

int
exec_local_cmd(char *cmd) {
    return system(cmd);
}

static struct command *
find_inner_command(char *name) {
    struct command *p = inner_commands;
    while (p) {
        if (strcmp(name, p->name) == 0) {
            return p;
        }
        p = p->next;
    }
    return NULL;
}

int
exec_inner_cmd(char *line) {
    char word[32];
    char *pword = word;
    int i = 0;
    struct command *cmd;
    char *argv;

    while (line[i] && whitespace(line[i]))
        i++;
    while (line[i] && !whitespace(line[i]) && i < 31) {
        *pword++ = line[i++];
    }
    *pword = '\0';
    if (i >= 31) {
        eprintf("%s: command is too long\n", word);
        return -1;
    }

    cmd = find_inner_command(word);
    if (!cmd) {
        printf("%s: No such command\n", word);
        return -1;
    }

    while (whitespace(line[i]))
        i++;

    argv = line + i;

    return (*cmd->func)(argv);
}

void
reap_child_handler(int sig) {
    int exit_code;
    int pid;
    int ret;
    struct slot *pslot;
    SAVE_ERRNO;
    while ((pid = waitpid(-1, &ret, WNOHANG)) > 0) {
        if (WIFEXITED(ret))
            exit_code = WEXITSTATUS(ret);
        else
            exit_code = 255;
        pslot = slot_find_by_pid(slots, pid);
        if (pslot) {
            slot_close(pslot, exit_code);
        }
        if (pconfig->verbose) {
            printf("wait pid %d, status code %d\n", pslot->pid, pslot->exit_code);
        }
        alive_children--;
    }

    RESTORE_ERRNO;
}