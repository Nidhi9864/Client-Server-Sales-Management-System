// parent.c - Head Office process that spawns branch child processes, communicates via FIFOs,
// and polls their responses concurrently.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#define MAX_BRANCHES 8
#define MAX_NAME 64
#define BUF 1024

typedef struct {
    char name[MAX_NAME];
    char fifo_parent_to_child[128];
    char fifo_child_to_parent[128];
    pid_t child_pid;
    int fd_w; // write to child
    int fd_r; // read from child
} Branch;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int mkfifo_if_needed(const char *path) {
    if (mkfifo(path, 0666) == -1) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

static void spawn_child(Branch *b) {
    // Ensure FIFOs exist
    if (mkfifo_if_needed(b->fifo_parent_to_child) == -1) die("mkfifo parent->child");
    if (mkfifo_if_needed(b->fifo_child_to_parent) == -1) die("mkfifo child->parent");

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        // Child: exec ./child <branchName> <p2c_fifo> <c2p_fifo> <data_dir>
        char data_dir[128];
        snprintf(data_dir, sizeof(data_dir), "data_%s", b->name);
        execl("./child", "child", b->name, b->fifo_parent_to_child, b->fifo_child_to_parent, data_dir, (char*)NULL);
        perror("execl child");
        _exit(127);
    }
    b->child_pid = pid;

    // Parent opens FIFOs
    b->fd_w = open(b->fifo_parent_to_child, O_WRONLY);
    if (b->fd_w == -1) die("open write to child");
    b->fd_r = open(b->fifo_child_to_parent, O_RDONLY | O_NONBLOCK);
    if (b->fd_r == -1) die("open read from child");
}

static void send_cmd(Branch *b, const char *cmd) {
    dprintf(b->fd_w, "%s\n", cmd);
}

static void broadcast(Branch *branches, int n, const char *cmd) {
    for (int i = 0; i < n; ++i) send_cmd(&branches[i], cmd);
}

static void close_branch(Branch *b) {
    if (b->fd_w != -1) close(b->fd_w);
    if (b->fd_r != -1) close(b->fd_r);
    // FIFOs are left in workspace; Makefile clean removes them.
}

int main(int argc, char *argv[]) {
    // Configure branches (can be edited or taken from argv in future)
    int n = 3;
    Branch branches[MAX_BRANCHES] = {0};
    const char *defaults[] = {"Ahmedabad", "Surat", "Vadodara"};
    for (int i = 0; i < n; ++i) {
        snprintf(branches[i].name, sizeof(branches[i].name), "%s", defaults[i]);
        snprintf(branches[i].fifo_parent_to_child, sizeof(branches[i].fifo_parent_to_child), "fifo_p2c_%s", defaults[i]);
        snprintf(branches[i].fifo_child_to_parent, sizeof(branches[i].fifo_child_to_parent), "fifo_c2p_%s", defaults[i]);
        branches[i].fd_r = branches[i].fd_w = -1;
    }

    printf("[Company] Launching %d branches...\n", n);
    for (int i = 0; i < n; ++i) spawn_child(&branches[i]);

    // Initial handshake
    for (int i = 0; i < n; ++i) send_cmd(&branches[i], "HELLO");

    // Demo script of commands to show IPC + concurrency
    // You can extend or make interactive later.
    broadcast(branches, n, "GET_SUMMARY");
    send_cmd(&branches[0], "RESTOCK shirts 10");
    send_cmd(&branches[1], "SALE jeans 5");
    send_cmd(&branches[2], "HIRE Anil Cashier");
    send_cmd(&branches[0], "SALE shirts 3");
    send_cmd(&branches[1], "RESTOCK jeans 7");
    broadcast(branches, n, "GET_STOCK");
    broadcast(branches, n, "GET_STAFF");
    send_cmd(&branches[2], "SALE shirts 2");
    send_cmd(&branches[2], "SALE jeans 1");
    broadcast(branches, n, "GET_SALES");
    broadcast(branches, n, "GET_SUMMARY");

    // Poll loop to read responses for ~10 seconds
    struct pollfd pfds[MAX_BRANCHES];
    char buf[BUF];
    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        int nfds = 0;
        for (int i = 0; i < n; ++i) {
            pfds[nfds].fd = branches[i].fd_r;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        int rv = poll(pfds, nfds, 500); // 0.5s
        if (rv > 0) {
            int idx = 0;
            for (int i = 0; i < n; ++i) {
                if (pfds[idx].revents & POLLIN) {
                    ssize_t r = read(pfds[idx].fd, buf, sizeof(buf)-1);
                    if (r > 0) {
                        buf[r] = 0;
                        // Print line by line with branch tag
                        char *saveptr;
                        char *line = strtok_r(buf, "\n", &saveptr);
                        while (line) {
                            printf("[%s -> Company] %s\n", branches[i].name, line);
                            line = strtok_r(NULL, "\n", &saveptr);
                        }
                    }
                }
                idx++;
            }
        }
    }

    // Graceful shutdown
    printf("[Company] Requesting graceful shutdown...\n");
    broadcast(branches, n, "EXIT");

    // Give children a moment to exit, then close resources
    sleep(1);
    for (int i = 0; i < n; ++i) {
        close_branch(&branches[i]);
        // Optionally wait on children (not strictly required since they are independent after IPC close)
    }
    printf("[Company] Done.\n");
    return 0;
}
