/* branch_server.c
   Usage: ./branch_server <BRANCH_ID> <CSV_FILE> <PORT>
   Example: ./branch_server A branchA.csv 5001
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define BACKLOG 5
#define BUF_SZ 4096

/* Read CSV file with header date,amount and compute subtotal and count */
int compute_subtotal(const char *csvfile, double *subtotal, int *count) {
    FILE *f = fopen(csvfile, "r");
    if (!f) return -1;
    char line[512];
    *subtotal = 0.0;
    *count = 0;
    /* skip header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ',');
        if (!p) continue;
        double amt = atof(p+1);
        *subtotal += amt;
        (*count)++;
    }
    fclose(f);
    return 0;
}

int start_server(const char *port) {
    struct addrinfo hints, *res, *rp;
    int sfd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      /* IPv4 only; change AF_UNSPEC to support IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &res) != 0) return -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;
        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (listen(sfd, BACKLOG) == 0) break;
        }
        close(sfd);
        sfd = -1;
    }
    freeaddrinfo(res);
    return sfd;
}

ssize_t robust_recv(int fd, void *buf, size_t count) {
    ssize_t r;
    while (1) {
        r = recv(fd, buf, count, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

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

int handle_client(int cfd, const char *branch_id, const char *csvfile) {
    char req[128];
    ssize_t r = robust_recv(cfd, req, sizeof(req)-1);
    if (r <= 0) return -1;
    req[r] = '\0';
    if (strstr(req, "REQUEST") == NULL) {
        // Unexpected; ignore
        return -1;
    }
    double subtotal = 0.0;
    int count = 0;
    if (compute_subtotal(csvfile, &subtotal, &count) != 0) {
        // send error
        const char *err = "ERROR: cannot read CSV\nEND\n";
        robust_send(cfd, err, strlen(err));
        return -1;
    }
    char out[512];
    snprintf(out, sizeof(out),
             "BRANCH_ID: %s\nRECORDS: %d\nSUBTOTAL: %.2f\nEND\n",
             branch_id, count, subtotal);
    robust_send(cfd, out, strlen(out));
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <BRANCH_ID> <CSV_FILE> <PORT>\n", argv[0]);
        return 1;
    }
    const char *branch_id = argv[1];
    const char *csvfile = argv[2];
    const char *port = argv[3];

    int sfd = start_server(port);
    if (sfd < 0) {
        perror("start_server");
        return 1;
    }
    printf("Branch %s server listening on port %s (CSV=%s)\n", branch_id, port, csvfile);

    while (1) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(sfd, (struct sockaddr*)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        /* handle client in the same process (single request per connection) */
        if (handle_client(cfd, branch_id, csvfile) != 0) {
            // error; already logged by client
        }
        close(cfd);
    }
    close(sfd);
    return 0;
}
