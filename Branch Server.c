// child.c - Branch process. Communicates via FIFOs, uses threads to simulate concurrent
// stock/staff/sales activity, and persists state to simple text files per-branch.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    int shirts;
    int jeans;
    int staff_count;
    int sales_count_shirts;
    int sales_count_jeans;
    pthread_mutex_t lock;
    char data_dir[128];
} State;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == -1 && errno != EEXIST) die("mkdir data_dir");
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Path exists but not a directory: %s\n", path);
        exit(EXIT_FAILURE);
    }
}

static void save_state(State *S) {
    // Persist minimal state to data files
    char path[256];
    pthread_mutex_lock(&S->lock);
    snprintf(path, sizeof(path), "%s/stock.txt", S->data_dir);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "shirts %d\njeans %d\n", S->shirts, S->jeans); fclose(f); }
    snprintf(path, sizeof(path), "%s/staff.txt", S->data_dir);
    f = fopen(path, "w");
    if (f) { fprintf(f, "staff_count %d\n", S->staff_count); fclose(f); }
    snprintf(path, sizeof(path), "%s/sales.txt", S->data_dir);
    f = fopen(path, "w");
    if (f) { fprintf(f, "shirts %d\njeans %d\n", S->sales_count_shirts, S->sales_count_jeans); fclose(f); }
    pthread_mutex_unlock(&S->lock);
}

static void load_state(State *S) {
    char path[256];
    int a,b;
    // Defaults
    S->shirts = 20;
    S->jeans = 20;
    S->staff_count = 5;
    S->sales_count_shirts = 0;
    S->sales_count_jeans = 0;

    snprintf(path, sizeof(path), "%s/stock.txt", S->data_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char item[32];
        while (fscanf(f, "%31s %d", item, &a) == 2) {
            if (strcmp(item, "shirts")==0) S->shirts = a;
            else if (strcmp(item, "jeans")==0) S->jeans = a;
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/staff.txt", S->data_dir);
    f = fopen(path, "r");
    if (f) {
        char key[32];
        if (fscanf(f, "%31s %d", key, &a)==2 && strcmp(key,"staff_count")==0) S->staff_count=a;
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/sales.txt", S->data_dir);
    f = fopen(path, "r");
    if (f) {
        char item[32];
        while (fscanf(f, "%31s %d", item, &a) == 2) {
            if (strcmp(item, "shirts")==0) S->sales_count_shirts = a;
            else if (strcmp(item, "jeans")==0) S->sales_count_jeans = a;
        }
        fclose(f);
    }
}

typedef struct {
    int fd_in;   // parent->child FIFO
    int fd_out;  // child->parent FIFO
    char branch[64];
    State *state;
    volatile int running;
} Context;

static void *background_sales(void *arg) {
    Context *C = (Context*)arg;
    while (C->running) {
        usleep(300 * 1000); // 0.3s
        pthread_mutex_t *L = &C->state->lock;
        pthread_mutex_lock(L);
        // Simulate occasional sale if stock available
        if (C->state->shirts > 0 && (rand() % 5 == 0)) {
            C->state->shirts--;
            C->state->sales_count_shirts++;
        }
        if (C->state->jeans > 0 && (rand() % 7 == 0)) {
            C->state->jeans--;
            C->state->sales_count_jeans++;
        }
        pthread_mutex_unlock(L);
    }
    return NULL;
}

static void *autosave_thread(void *arg) {
    Context *C = (Context*)arg;
    while (C->running) {
        usleep(800 * 1000); // 0.8s
        save_state(C->state);
    }
    return NULL;
}

static void reply(Context *C, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dprintf(C->fd_out, "[%s] ", C->branch);
    vdprintf(C->fd_out, fmt, ap);
    dprintf(C->fd_out, "\n");
    va_end(ap);
}

static void handle_command(Context *C, char *line) {
    char cmd[32]={0}, arg1[64]={0}, arg2[64]={0};
    int n = sscanf(line, "%31s %63s %63s", cmd, arg1, arg2);
    if (n <= 0) return;

    if (strcmp(cmd, "HELLO")==0) {
        reply(C, "Hello from %s.", C->branch);
        return;
    }

    if (strcmp(cmd, "GET_STOCK")==0) {
        pthread_mutex_lock(&C->state->lock);
        reply(C, "Stock -> shirts=%d, jeans=%d", C->state->shirts, C->state->jeans);
        pthread_mutex_unlock(&C->state->lock);
        return;
    }

    if (strcmp(cmd, "RESTOCK")==0 && n==3) {
        int qty = atoi(arg2);
        pthread_mutex_lock(&C->state->lock);
        if (strcmp(arg1, "shirts")==0) C->state->shirts += qty;
        else if (strcmp(arg1, "jeans")==0) C->state->jeans += qty;
        pthread_mutex_unlock(&C->state->lock);
        reply(C, "Restocked %s by %d.", arg1, qty);
        return;
    }

    if (strcmp(cmd, "SALE")==0 && n==3) {
        int qty = atoi(arg2), ok=1;
        pthread_mutex_lock(&C->state->lock);
        if (strcmp(arg1,"shirts")==0) {
            if (C->state->shirts >= qty) { C->state->shirts -= qty; C->state->sales_count_shirts += qty; }
            else ok=0;
        } else if (strcmp(arg1,"jeans")==0) {
            if (C->state->jeans >= qty) { C->state->jeans -= qty; C->state->sales_count_jeans += qty; }
            else ok=0;
        } else ok=0;
        pthread_mutex_unlock(&C->state->lock);
        if (ok) reply(C, "Sale recorded: %s %d.", arg1, qty);
        else reply(C, "Sale failed for %s %d (insufficient stock or bad item).", arg1, qty);
        return;
    }

    if (strcmp(cmd, "GET_SALES")==0) {
        pthread_mutex_lock(&C->state->lock);
        reply(C, "Sales -> shirts=%d, jeans=%d", C->state->sales_count_shirts, C->state->sales_count_jeans);
        pthread_mutex_unlock(&C->state->lock);
        return;
    }

    if (strcmp(cmd, "HIRE")==0 && n==3) {
        pthread_mutex_lock(&C->state->lock);
        C->state->staff_count += 1;
        pthread_mutex_unlock(&C->state->lock);
        reply(C, "Hired %s. Staff now %d.", arg1, C->state->staff_count);
        return;
    }

    if (strcmp(cmd, "GET_STAFF")==0) {
        pthread_mutex_lock(&C->state->lock);
        reply(C, "Staff count -> %d", C->state->staff_count);
        pthread_mutex_unlock(&C->state->lock);
        return;
    }

    if (strcmp(cmd, "GET_SUMMARY")==0) {
        pthread_mutex_lock(&C->state->lock);
        reply(C, "Summary :: stock(shirts=%d, jeans=%d), staff=%d, sales(shirts=%d, jeans=%d)",
              C->state->shirts, C->state->jeans, C->state->staff_count,
              C->state->sales_count_shirts, C->state->sales_count_jeans);
        pthread_mutex_unlock(&C->state->lock);
        return;
    }

    if (strcmp(cmd, "EXIT")==0) {
        reply(C, "Shutting down gracefully.");
        // running flag will be cleared by caller loop
        return;
    }

    reply(C, "Unknown or malformed command: %s", line);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <branchName> <p2c_fifo> <c2p_fifo> <data_dir>\n", argv[0]);
        return 1;
    }
    const char *branch = argv[1];
    const char *fifo_in = argv[2];
    const char *fifo_out = argv[3];
    const char *data_dir = argv[4];

    srand((unsigned)time(NULL) ^ getpid());

    int fd_in = open(fifo_in, O_RDONLY);
    if (fd_in == -1) die("child open fifo_in");
    int fd_out = open(fifo_out, O_WRONLY);
    if (fd_out == -1) die("child open fifo_out");

    State S;
    memset(&S, 0, sizeof(S));
    pthread_mutex_init(&S.lock, NULL);
    snprintf(S.data_dir, sizeof(S.data_dir), "%s", data_dir);

    ensure_dir(S.data_dir);
    load_state(&S); // initialize from disk if present

    Context C = { .fd_in = fd_in, .fd_out = fd_out, .state = &S, .running = 1 };
    snprintf(C.branch, sizeof(C.branch), "%s", branch);

    // Start background threads
    pthread_t th_sales, th_autosave;
    pthread_create(&th_sales, NULL, background_sales, &C);
    pthread_create(&th_autosave, NULL, autosave_thread, &C);

    // Command loop: read lines from parent and process
    char line[1024];
    while (C.running) {
        ssize_t r = 0;
        int pos = 0;
        // Read until newline
        while (1) {
            char ch;
            r = read(fd_in, &ch, 1);
            if (r <= 0) { usleep(50*1000); continue; }
            if (ch == '\n') break;
            if (pos < (int)sizeof(line)-1) line[pos++] = ch;
        }
        line[pos] = 0;

        if (strncmp(line, "EXIT", 4) == 0) {
            handle_command(&C, line);
            C.running = 0;
            break;
        }
        handle_command(&C, line);
    }

    // Cleanup
    save_state(&S);
    pthread_mutex_destroy(&S.lock);
    // Stop threads
    C.running = 0;
    pthread_join(th_sales, NULL);
    pthread_join(th_autosave, NULL);

    close(fd_in);
    close(fd_out);
    return 0;
}
