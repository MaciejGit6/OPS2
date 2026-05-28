#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <semaphore.h>

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression)             \
    (__extension__({                               \
        long int __result;                         \
        do                                         \
            __result = (long int)(expression);     \
        while (__result == -1L && errno == EINTR); \
        __result;                                  \
    }))
#endif

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

void ms_sleep(unsigned int milli) {
    struct timespec ts = {milli / 1000, (milli % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int make_socket(int domain, int type) {
    int sock = socket(domain, type, 0);
    if (sock < 0) ERR("socket");
    return sock;
}

int bind_inet_socket(uint16_t port, int type, int backlog) {
    struct sockaddr_in addr;
    int socketfd, t = 1;
    socketfd = make_socket(PF_INET, type);
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t))) ERR("setsockopt");
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) ERR("bind");
    if (SOCK_STREAM == type)
        if (listen(socketfd, backlog) < 0) ERR("listen");
    return socketfd;
}

#define STACK_SIZE 16
#define DIVISION_NAMES_SIZE 128
#define MAP_SIZE 100

typedef struct {
    int x, y, p;
    char name[128];
    struct sockaddr_in addr;
    socklen_t addr_len;
} Report;

typedef struct {
    volatile sig_atomic_t do_work;
    int server_socket;
    uint16_t port;

    Report stack[STACK_SIZE];
    int stack_count, stack_head, stack_tail;
    sem_t sem_empty; 
    sem_t sem_full;  
    pthread_mutex_t stack_mutex;

    char div_names[DIVISION_NAMES_SIZE][128];
    struct sockaddr_in div_addrs[DIVISION_NAMES_SIZE];
    socklen_t div_addr_lens[DIVISION_NAMES_SIZE];
    int div_allegiance[DIVISION_NAMES_SIZE];
    int div_x[DIVISION_NAMES_SIZE];
    int div_y[DIVISION_NAMES_SIZE];
    int div_count;
    pthread_mutex_t div_mutex;

    int hq_map[MAP_SIZE][MAP_SIZE];
    pthread_mutex_t row_mutexes[MAP_SIZE];
} SharedState;

void* network_listener_thread(void* arg) {
    SharedState* shared = (SharedState*)arg;
    char buffer[256];

    while (shared->do_work) {
        Report msg;
        msg.addr_len = sizeof(msg.addr);
        
        int n = recvfrom(shared->server_socket, buffer, sizeof(buffer) - 1, 0, 
                         (struct sockaddr*)&msg.addr, &msg.addr_len);
        
        if (!shared->do_work) break; 
        
        if (n < 0) {
            if (errno == EINTR) continue;
            ERR("recvfrom");
        }
        buffer[n] = '\0';

        if (sscanf(buffer, "%d %d %d %127[^\n]", &msg.x, &msg.y, &msg.p, msg.name) == 4) {
            if (msg.x < 0 || msg.x >= MAP_SIZE || msg.y < 0 || msg.y >= MAP_SIZE || (msg.p != 0 && msg.p != 1)) {
                fprintf(stderr, "Error: Invalid map coordinate or allegiance.\n");
                continue;
            }

            sem_wait(&shared->sem_empty);
            if (!shared->do_work) break;

            pthread_mutex_lock(&shared->stack_mutex);
            shared->stack[shared->stack_tail] = msg;
            shared->stack_tail = (shared->stack_tail + 1) % STACK_SIZE;
            shared->stack_count++;
            pthread_mutex_unlock(&shared->stack_mutex);

            sem_post(&shared->sem_full);
        } else {
            fprintf(stderr, "Error: Malformed message received.\n");
        }
    }
    return NULL;
}

void* adjutant_thread(void* arg) {
    SharedState* shared = (SharedState*)arg;

    while (shared->do_work) {
        sem_wait(&shared->sem_full);
        if (!shared->do_work) break; 
        
        pthread_mutex_lock(&shared->stack_mutex);
        Report rep = shared->stack[shared->stack_head];
        shared->stack_head = (shared->stack_head + 1) % STACK_SIZE;
        shared->stack_count--;
        pthread_mutex_unlock(&shared->stack_mutex);

        sem_post(&shared->sem_empty);

        printf("%s division %s was seen at position %d:%d.\n", 
               rep.p == 1 ? "allied" : "enemy", rep.name, rep.x, rep.y);

        ms_sleep(10); 

        int div_id = -1;
        
        pthread_mutex_lock(&shared->div_mutex);
        for (int i = 0; i < shared->div_count; i++) {
            if (strcmp(shared->div_names[i], rep.name) == 0) {
                div_id = i;
                break;
            }
        }
        
        if (div_id == -1 && shared->div_count < DIVISION_NAMES_SIZE) {
            div_id = shared->div_count++;
            strcpy(shared->div_names[div_id], rep.name);
            shared->div_x[div_id] = -1; 
            shared->div_y[div_id] = -1;
        }

        if (div_id != -1) {
            shared->div_allegiance[div_id] = rep.p;
            shared->div_addrs[div_id] = rep.addr;
            shared->div_addr_lens[div_id] = rep.addr_len;
            
            int old_x = shared->div_x[div_id];
            int old_y = shared->div_y[div_id];
            
            shared->div_x[div_id] = rep.x;
            shared->div_y[div_id] = rep.y;
            pthread_mutex_unlock(&shared->div_mutex);

            if (old_y != -1 && old_x != -1) {
                pthread_mutex_lock(&shared->row_mutexes[old_y]);
                if (shared->hq_map[old_y][old_x] == div_id) { 
                    shared->hq_map[old_y][old_x] = -1; 
                }
                pthread_mutex_unlock(&shared->row_mutexes[old_y]);
            }

            pthread_mutex_lock(&shared->row_mutexes[rep.y]);
            shared->hq_map[rep.y][rep.x] = div_id; 
            pthread_mutex_unlock(&shared->row_mutexes[rep.y]);
        } else {
            pthread_mutex_unlock(&shared->div_mutex); 
        }
    }
    return NULL;
}

void* napoleon_thread(void* arg) {
    SharedState* shared = (SharedState*)arg;
    unsigned int seed = time(NULL);
    
    while (shared->do_work) {
        ms_sleep(30);

        printf("--- Emperor's Map Report ---\n");
        pthread_mutex_lock(&shared->div_mutex);
        int allied_count = 0;
        int allied_ids[DIVISION_NAMES_SIZE];

        for (int i = 0; i < shared->div_count; i++) {
            if (shared->div_x[i] != -1 && shared->div_y[i] != -1) {
                printf("[%s] %s at %d:%d\n", 
                    shared->div_allegiance[i] == 1 ? "ALLIED" : "ENEMY ", 
                    shared->div_names[i], shared->div_x[i], shared->div_y[i]);
            }
            if (shared->div_allegiance[i] == 1) {
                allied_ids[allied_count++] = i;
            }
        }
        
        if (allied_count > 0) {
            int target_idx = allied_ids[rand_r(&seed) % allied_count];
            struct sockaddr_in target_addr = shared->div_addrs[target_idx];
            socklen_t target_len = shared->div_addr_lens[target_idx];
            char target_name[128];
            strcpy(target_name, shared->div_names[target_idx]);
            pthread_mutex_unlock(&shared->div_mutex);
            
            int order_x = rand_r(&seed) % MAP_SIZE;
            int order_y = rand_r(&seed) % MAP_SIZE;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%d %d 1 %s\n", order_x, order_y, target_name);
            
            sendto(shared->server_socket, buffer, strlen(buffer), 0, (struct sockaddr*)&target_addr, target_len);
        } else {
            pthread_mutex_unlock(&shared->div_mutex);
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    SharedState shared;
    memset(&shared, 0, sizeof(SharedState));
    shared.port = atoi(argv[1]);
    shared.do_work = 1;

    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0) ERR("pthread_sigmask");

    sem_init(&shared.sem_empty, 0, STACK_SIZE);
    sem_init(&shared.sem_full, 0, 0);
    pthread_mutex_init(&shared.stack_mutex, NULL);
    pthread_mutex_init(&shared.div_mutex, NULL);

    for (int y = 0; y < MAP_SIZE; y++) {
        pthread_mutex_init(&shared.row_mutexes[y], NULL);
        for (int x = 0; x < MAP_SIZE; x++) {
            shared.hq_map[y][x] = -1;
        }
    }

    shared.server_socket = bind_inet_socket(shared.port, SOCK_DGRAM, 0);

    pthread_t listener;
    pthread_t adjutants[4];
    pthread_t napoleon;
    
    pthread_create(&listener, NULL, network_listener_thread, &shared);
    for (int i = 0; i < 4; i++) {
        pthread_create(&adjutants[i], NULL, adjutant_thread, &shared);
    }
    pthread_create(&napoleon, NULL, napoleon_thread, &shared);

    printf("Headquarters established on port %d. Waiting for messengers...\n", shared.port);
    printf("Press Ctrl+C to safely shutdown.\n");

    int caught_sig;
    sigwait(&sigset, &caught_sig);

    printf("\nReceived SIGINT. Shutting down headquarters safely...\n");

    shared.do_work = 0;

    struct sockaddr_in localhost;
    memset(&localhost, 0, sizeof(localhost));
    localhost.sin_family = AF_INET;
    localhost.sin_port = htons(shared.port);
    localhost.sin_addr.s_addr = htonl(INADDR_LOOPBACK); 
    sendto(shared.server_socket, "SHUT", 4, 0, (struct sockaddr*)&localhost, sizeof(localhost));

    for (int i = 0; i < 4; i++) sem_post(&shared.sem_full);
    
    sem_post(&shared.sem_empty);

    pthread_join(listener, NULL);
    for (int i = 0; i < 4; i++) {
        pthread_join(adjutants[i], NULL);
    }
    pthread_join(napoleon, NULL);

    close(shared.server_socket);
    sem_destroy(&shared.sem_empty);
    sem_destroy(&shared.sem_full);
    pthread_mutex_destroy(&shared.stack_mutex);
    pthread_mutex_destroy(&shared.div_mutex);
    for (int y = 0; y < MAP_SIZE; y++) {
        pthread_mutex_destroy(&shared.row_mutexes[y]);
    }

    printf("Cleanup complete. Farewell.\n");
    return 0;
}