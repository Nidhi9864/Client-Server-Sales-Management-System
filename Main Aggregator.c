/* main_aggregator.c
   Usage: ./main_aggregator <MAIN_CSV> <BRANCH1_HOST> <BRANCH1_PORT> <BRANCH2_HOST> <BRANCH2_PORT>
   Example: ./main_aggregator main.csv localhost 5001 localhost 5002
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/time.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define BUF_SZ 4096
#define TIMEOUT_SEC 5

ssize_t robust_send(int fd, const void *buf, size_t count) {
    size_t sent = 0;
    while (sent < count) {
        ssize_t r = send(fd, (const char*)buf + sent, count - sent, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += r;
    }
    return sent;
}

ssize_t robust_recv(int fd, void *buf, size_t count) {
    ssize_t r;
    while (1) {
        r = recv(fd, buf, count, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

/* Connect to host:port -> returns socket fd or -1 */
int connect_to(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    int sfd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 for simplicity
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sfd);
        sfd = -1;
    }
    freeaddrinfo(res);
    return sfd;
}

/* Parse branch reply text and extract values */
int parse_reply(const char *reply, char *branch_id, size_t bid_len, int *records, double *subtotal) {
    const char *p = strstr(reply, "BRANCH_ID:");
    if (!p) return -1;
    if (sscanf(p, "BRANCH_ID: %s", branch_id) != 1) return -1;
    p = strstr(reply, "RECORDS:");
    if (!p) return -1;
    if (sscanf(p, "RECORDS: %d", records) != 1) return -1;
    p = strstr(reply, "SUBTOTAL:");
    if (!p) return -1;
    if (sscanf(p, "SUBTOTAL: %lf", subtotal) != 1) return -1;
    return 0;
}

/* produce ISO8601 timestamp */
void iso_time(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Atomically append an entry to main CSV: read-append-write via temp + rename, with flock for safety */
int update_main_csv(const char *main_csv, const char *branch_id, int records, double subtotal) {
    FILE *f = fopen(main_csv, "r");
    if (!f) {
        perror("fopen main csv");
        return -1;
    }
    /* We will use flock on the file descriptor */
    int fd = fileno(f);
    if (flock(fd, LOCK_EX) != 0) {
        perror("flock");
        fclose(f);
        return -1;
    }
    /* Read existing content to memory (small file expected) */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *contents = malloc(size + 1);
    if (!contents) { flock(fd, LOCK_UN); fclose(f); return -1; }
    fread(contents, 1, size, f);
    contents[size] = '\0';

    /* Create temp file */
    char tmpname[512];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", main_csv);
    FILE *tf = fopen(tmpname, "w");
    if (!tf) { perror("fopen tmp"); free(contents); flock(fd, LOCK_UN); fclose(f); return -1; }

    /* write old content and append new line */
    fputs(contents, tf);
    free(contents);

    char timestr[64];
    iso_time(timestr, sizeof(timestr));
    fprintf(tf, "%s,%s,%d,%.2f,%s\n", timestr, branch_id, records, subtotal, timestr);
    fflush(tf);
    fsync(fileno(tf));
    fclose(tf);

    /* rename atomically */
    if (rename(tmpname, main_csv) != 0) {
        perror("rename");
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    flock(fd, LOCK_UN);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <MAIN_CSV> <B1_HOST> <B1_PORT> <B2_HOST> <B2_PORT>\n", argv[0]);
        return 1;
    }
    const char *main_csv = argv[1];
    const char *b1_host = argv[2];
    const char *b1_port = argv[3];
    const char *b2_host = argv[4];
    const char *b2_port = argv[5];

    int s1 = connect_to(b1_host, b1_port);
    if (s1 < 0) { fprintf(stderr, "Could not connect to branch1 %s:%s\n", b1_host, b1_port); }
    int s2 = connect_to(b2_host, b2_port);
    if (s2 < 0) { fprintf(stderr, "Could not connect to branch2 %s:%s\n", b2_host, b2_port); }

    if (s1 < 0 && s2 < 0) {
        fprintf(stderr, "No branches available. Exiting.\n");
        return 1;
    }

    /* Send REQUEST to connected branches */
    const char *req = "REQUEST\n";
    if (s1 >= 0) robust_send(s1, req, strlen(req));
    if (s2 >= 0) robust_send(s2, req, strlen(req));

    /* Use select to wait for both sockets with TIMEOUT_SEC timeout */
    fd_set readfds;
    int maxfd = (s1 > s2 ? s1 : s2);
    char buf[BUF_SZ+1];
    if (maxfd < 0) maxfd = 0;

    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;

    int remaining = 0;
    if (s1 >= 0) remaining++;
    if (s2 >= 0) remaining++;

    while (remaining > 0) {
        FD_ZERO(&readfds);
        if (s1 >= 0) FD_SET(s1, &readfds);
        if (s2 >= 0) FD_SET(s2, &readfds);

        struct timeval tv2 = tv; /* select may modify */
        int rv = select(maxfd + 1, &readfds, NULL, NULL, &tv2);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        } else if (rv == 0) {
            fprintf(stderr, "Timeout waiting for branches.\n");
            break;
        }

        for (int s = 0; s <= maxfd; ++s) {
            if ((s == s1 || s == s2) && FD_ISSET(s, &readfds)) {
                ssize_t r = robust_recv(s, buf, BUF_SZ);
                if (r <= 0) {
                    // connection closed or error
                    close(s);
                    if (s == s1) s1 = -1;
                    if (s == s2) s2 = -1;
                    remaining--;
                    continue;
                }
                buf[r] = '\0';
                // we expect the branch to send full message in one shot (small)
                char branch_id[64];
                int records = 0;
                double subtotal = 0.0;
                if (parse_reply(buf, branch_id, sizeof(branch_id), &records, &subtotal) == 0) {
                    printf("Received from %s: records=%d subtotal=%.2f\n", branch_id, records, subtotal);
                    if (update_main_csv(main_csv, branch_id, records, subtotal) == 0) {
                        printf("main CSV updated for branch %s\n", branch_id);
                    } else {
                        fprintf(stderr, "Failed to update main CSV for branch %s\n", branch_id);
                    }
                } else {
                    fprintf(stderr, "Malformed reply from socket %d: [%s]\n", s, buf);
                }
                close(s);
                if (s == s1) s1 = -1;
                if (s == s2) s2 = -1;
                remaining--;
            }
        } /* end for */
    } /* end while */
    printf("Aggregator finished.\n");
    return 0;
}
