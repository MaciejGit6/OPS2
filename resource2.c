#include "header.h"

#define BUFFER_SIZE 1024
#define RING_SIZE 32
#define WORKER_COUNT 4

volatile sig_atomic_t do_work = 1;

typedef struct {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len;
} packet_t;

typedef struct {
    packet_t buf[RING_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mtx;
    sem_t sem;
} ring_buffer_t;

typedef struct {
    int server_fd;
    ring_buffer_t* queue;
} worker_args_t;

void sigint_handler(int sig) {
    (void)sig;
    do_work = 0;
}

void init_ring(ring_buffer_t* rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;

    pthread_mutex_init(&rb->mtx, NULL);
    sem_init(&rb->sem, 0, 0);
}

int push_ring(ring_buffer_t* rb, packet_t pkt) {
    int success = 0;

    pthread_mutex_lock(&rb->mtx);

    if (rb->count < RING_SIZE) {
        rb->buf[rb->tail] = pkt;
        rb->tail = (rb->tail + 1) % RING_SIZE;
        rb->count++;
        success = 1;
    }

    pthread_mutex_unlock(&rb->mtx);

    if (success) {
        sem_post(&rb->sem);
    }

    return success;
}

int pop_ring(ring_buffer_t* rb, packet_t* out_pkt) {
    sem_wait(&rb->sem);

    if (!do_work && rb->count == 0) {
        return 0;
    }

    pthread_mutex_lock(&rb->mtx);

    *out_pkt = rb->buf[rb->head];
    rb->head = (rb->head + 1) % RING_SIZE;
    rb->count--;

    pthread_mutex_unlock(&rb->mtx);

    return 1;
}

void* worker_task(void* arg) {
    worker_args_t* ctx = (worker_args_t*)arg;

    while (1) {
        packet_t pkt;

        if (!pop_ring(ctx->queue, &pkt)) {
            break;
        }

        char* saveptr;
        int token_count = 0;

        char* token = strtok_r(pkt.buffer, " \n", &saveptr);

        while (token != NULL) {
            token_count++;
            token = strtok_r(NULL, " \n", &saveptr);
        }

        char response[BUFFER_SIZE];

        snprintf(
            response,
            sizeof(response),
            "ACK: Parsed %d tokens\n",
            token_count
        );

        TEMP_FAILURE_RETRY(
            sendto(
                ctx->server_fd,
                response,
                strlen(response),
                0,
                (struct sockaddr*)&pkt.client_addr,
                pkt.client_len
            )
        );
    }

    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    uint16_t port = atoi(argv[1]);

    if (sethandler(sigint_handler, SIGINT) < 0) {
        ERR("sethandler");
    }

    ring_buffer_t queue;
    init_ring(&queue);

    int server_fd = bind_inet_socket(port, SOCK_DGRAM, 0);

    worker_args_t ctx;
    ctx.server_fd = server_fd;
    ctx.queue = &queue;

    pthread_t workers[WORKER_COUNT];

    for (int i = 0; i < WORKER_COUNT; i++) {
        if (pthread_create(&workers[i], NULL, worker_task, &ctx) != 0) {
            ERR("pthread_create");
        }
    }

    while (do_work) {
        packet_t new_pkt;

        new_pkt.client_len = sizeof(new_pkt.client_addr);

        ssize_t n = TEMP_FAILURE_RETRY(
            recvfrom(
                server_fd,
                new_pkt.buffer,
                BUFFER_SIZE - 1,
                0,
                (struct sockaddr*)&new_pkt.client_addr,
                &new_pkt.client_len
            )
        );

        if (n < 0 && do_work == 0) {
            break;
        }

        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        new_pkt.buffer[n] = '\0';

        push_ring(&queue, new_pkt);
    }

    for (int i = 0; i < WORKER_COUNT; i++) {
        sem_post(&queue.sem);
    }

    for (int i = 0; i < WORKER_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }

    close(server_fd);

    pthread_mutex_destroy(&queue.mtx);
    sem_destroy(&queue.sem);

    return EXIT_SUCCESS;
}