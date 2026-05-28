#include "l8_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SPELL_TYPES 3
const char* spell_names[SPELL_TYPES] = {"Divination", "Summon Elemental", "Fireball"};
const int spell_costs[SPELL_TYPES] = {1, 3, 4}; 

#define BOARD_SIZE 8
#define BACKLOG 16

#define MAX_QUEUE 10
#define THREAD_COUNT 3
#define FAMILIAR_DELAY 100

#define MAX_CLIENTS 2
#define MAX_NAME_LENGTH 14
#define MAX_BUFF_LEN 16

typedef struct message_t {
    char buff[16];
} message_t;

typedef struct __attribute__((__packed__)) cast_message_t {
    char type;
    char padding;
    uint16_t spell;
    uint16_t X;
    uint16_t Y;
} cast_message_t;

typedef struct __attribute__((__packed__)) quit_message_t {
    char type;
} quit_message_t;

typedef struct __attribute__((__packed__)) login_message_t {
    char type;
    char padding;
    char name[MAX_NAME_LENGTH + 1];
} login_message_t;

typedef struct {
    cast_message_t buff[MAX_QUEUE];
    pthread_mutex_t mutex;
    sem_t sem;
    int head;
    int tail;
    int count;
} queue_t;

typedef struct {
    struct sockaddr_in addr;
    char name[MAX_NAME_LENGTH + 1];
    int pebbles;
    int logged;
} beloved_t;

typedef struct {
    beloved_t Misha;
    beloved_t Zosia;
    int started;
    int game_over;
    int server_fd;
    int board[BOARD_SIZE][BOARD_SIZE]; 
    queue_t queue;
    pthread_mutex_t mutex;
} gamestate_t;

void queue_init(queue_t* queue) {
    pthread_mutex_init(&queue->mutex, NULL);
    sem_init(&queue->sem, 0, 0);
    queue->head = queue->tail = queue->count = 0;
}

void queue_destroy(queue_t* queue) {
    pthread_mutex_destroy(&queue->mutex);
    sem_destroy(&queue->sem);
}

void queue_push(queue_t* queue, cast_message_t* message) {
    pthread_mutex_lock(&queue->mutex);
    if (queue->count < MAX_QUEUE) {
        memcpy(&queue->buff[queue->tail], message, sizeof(cast_message_t));
        queue->tail = (queue->tail + 1) % MAX_QUEUE;
        queue->count++;
        sem_post(&queue->sem);
    } else {
        printf("Queue is full, discarding cast message.\n");
    }
    pthread_mutex_unlock(&queue->mutex);
}

cast_message_t queue_pop(queue_t* queue) {
    sem_wait(&queue->sem);
    pthread_mutex_lock(&queue->mutex);
    cast_message_t message = queue->buff[queue->head];
    queue->head = (queue->head + 1) % MAX_QUEUE;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return message;
}

void gamestate_init(gamestate_t* game, int fd) {
    game->Misha.name[0] = 0;
    game->Zosia.name[0] = 0;
    game->Misha.pebbles = 10;
    game->Zosia.pebbles = 10;
    game->Zosia.logged = 0;
    game->Misha.logged = 0;
    game->started = 0;
    game->game_over = 0;
    game->server_fd = fd;
    memset(game->board, 0, sizeof(game->board));
    pthread_mutex_init(&game->mutex, NULL);
    queue_init(&game->queue);
}

void add_beloved(beloved_t beloved, gamestate_t* game) {
    if ((game->Misha.logged && !memcmp(&game->Misha.addr, (struct sockaddr_in*)&beloved.addr, sizeof(struct sockaddr_in))) ||
        (game->Zosia.logged && !memcmp(&game->Zosia.addr, (struct sockaddr_in*)&beloved.addr, sizeof(struct sockaddr_in)))) {
        printf("Rejecting the beloved because of self-love\n");
        return;
    }
    if (!game->Misha.logged) {
        game->Misha = beloved;
        printf("Misha joined\n");
    } else if (!game->Zosia.logged) {
        game->Zosia = beloved;
        game->started = 1;
        printf("Zosia joined\n");
    } else {
        printf("We like monogamy\n");
    }
}

int is_logged_in(gamestate_t* game, struct sockaddr_in addr) {
    if (game->Misha.logged && !memcmp(&game->Misha.addr, &addr, sizeof(addr))) return 1;
    else if (game->Zosia.logged && !memcmp(&game->Zosia.addr, &addr, sizeof(addr))) return 1;
    return 0;
}

int get_beloved_index(gamestate_t* game, struct sockaddr_in addr) {
    if (game->Misha.logged && !memcmp(&game->Misha.addr, &addr, sizeof(addr))) return 0;
    else if (game->Zosia.logged && !memcmp(&game->Zosia.addr, &addr, sizeof(addr))) return 1;
    return -1;
}

void* worker(void* args) {
    gamestate_t* game = args;

    while (1) {
        cast_message_t message = queue_pop(&game->queue);
        
        pthread_mutex_lock(&game->mutex);
        if (game->game_over) {
            pthread_mutex_unlock(&game->mutex);
            break;
        }

        beloved_t* player = (message.padding == 0) ? &game->Misha : &game->Zosia;
        int player_id = (message.padding == 0) ? 1 : 2;

        if (player->pebbles < spell_costs[message.spell]) {
            printf("[tee hee] Not enough pebbles, <%s>!\n", player->name);
            pthread_mutex_unlock(&game->mutex);
            usleep(FAMILIAR_DELAY * 1000);
            continue;
        }

        player->pebbles -= spell_costs[message.spell];
        int cx = message.X;
        int cy = message.Y;

        if (message.spell == 0) {
            uint16_t div_resp[25];
            int idx = 0;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    uint16_t val;
                    if (nx < 0 || nx >= BOARD_SIZE || ny < 0 || ny >= BOARD_SIZE) val = 3;
                    else if (game->board[ny][nx] == 0) val = 0;
                    else if (game->board[ny][nx] == player_id) val = 1;
                    else val = 2;
                    div_resp[idx++] = htons(val);
                }
            }
            sendto(game->server_fd, div_resp, sizeof(div_resp), 0, (struct sockaddr*)&player->addr, sizeof(player->addr));
        } 
        else if (message.spell == 1) { 
            if (game->board[cy][cx] == 0) {
                game->board[cy][cx] = player_id;
            }
        } 
        else if (message.spell == 2) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx >= 0 && nx < BOARD_SIZE && ny >= 0 && ny < BOARD_SIZE) {
                        game->board[ny][nx] = 0;
                    }
                }
            }
        }

        printf("[Cast] <%s> casts <%s> onto <%hu>,<%hu>\n", player->name, spell_names[message.spell], message.X, message.Y);
        
        uint16_t net_pebbles = htons((uint16_t)player->pebbles);
        sendto(game->server_fd, &net_pebbles, sizeof(net_pebbles), 0, (struct sockaddr*)&player->addr, sizeof(player->addr));
        
        pthread_mutex_unlock(&game->mutex);
        usleep(FAMILIAR_DELAY * 1000);
    }
    return NULL;
}

void* judge_thread(void* args) {
    gamestate_t* game = args;

    while (1) {
        usleep(1000 * 1000); // 1 second
        
        pthread_mutex_lock(&game->mutex);
        if (game->game_over) {
            pthread_mutex_unlock(&game->mutex);
            break;
        }
        if (!game->started) {
            pthread_mutex_unlock(&game->mutex);
            continue;
        }

        printf("\n--- Judge's Report ---\n");
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                if (game->board[y][x] == 0) printf(". ");
                else if (game->board[y][x] == 1) printf("M ");
                else printf("Z ");
            }
            printf("\n");
        }
        printf("Legend: M - %s | Z - %s\n", game->Misha.name, game->Zosia.name);

        int n_misha = 0, n_zosia = 0;
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                if (game->board[y][x] == 1) n_misha++;
                if (game->board[y][x] == 2) n_zosia++;
            }
        }

        int dk_misha = (n_misha / 2) - 1;
        int dk_zosia = (n_zosia / 2) - 1;

        game->Misha.pebbles += dk_misha;
        game->Zosia.pebbles += dk_zosia;

        printf("Pouches | %s: %d pebbles | %s: %d pebbles\n", game->Misha.name, game->Misha.pebbles, game->Zosia.name, game->Zosia.pebbles);

        uint16_t p_m = htons((uint16_t)game->Misha.pebbles);
        sendto(game->server_fd, &p_m, 2, 0, (struct sockaddr*)&game->Misha.addr, sizeof(game->Misha.addr));
        uint16_t p_z = htons((uint16_t)game->Zosia.pebbles);
        sendto(game->server_fd, &p_z, 2, 0, (struct sockaddr*)&game->Zosia.addr, sizeof(game->Zosia.addr));

        int m_dead = (game->Misha.pebbles <= 0 && dk_misha < 0);
        int z_dead = (game->Zosia.pebbles <= 0 && dk_zosia < 0);

        if (m_dead || z_dead) {
            beloved_t* winner = (m_dead && z_dead) ? &game->Misha : (m_dead ? &game->Zosia : &game->Misha);
            beloved_t* loser = (m_dead && z_dead) ? &game->Zosia : (m_dead ? &game->Misha : &game->Zosia);

            printf("-= Congratulations, <%s>, you win! =-\n", winner->name);
            char w = 'w', l = 'l';
            sendto(game->server_fd, &w, 1, 0, (struct sockaddr*)&winner->addr, sizeof(winner->addr));
            sendto(game->server_fd, &l, 1, 0, (struct sockaddr*)&loser->addr, sizeof(loser->addr));
            game->game_over = 1;
        }
        pthread_mutex_unlock(&game->mutex);
    }
    return NULL;
}

void doServer(int fd) {
    pthread_t threads[THREAD_COUNT];
    pthread_t judge;

    gamestate_t game;
    gamestate_init(&game, fd);

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, worker, &game) < 0)
            perror("pthread_create worker");
    }
    if (pthread_create(&judge, NULL, judge_thread, &game) < 0) {
        perror("pthread_create judge");
    }

    struct sockaddr_in addr;
    socklen_t addr_len;
    char buff[MAX_BUFF_LEN + 1];

    while (!game.game_over) {
        addr_len = sizeof(addr);
        memset(buff, 0, sizeof(buff));
        
        int message_len;
        if ((message_len = recvfrom(fd, buff, MAX_BUFF_LEN, 0, (struct sockaddr*)&addr, &addr_len)) < 0) {
            perror("recvfrom");
            continue;
        }

        if (message_len < 16) {
            printf("message of incorrect length\n");
            continue;
        }

        if (buff[0] == 'l') {
            if (game.started) {
                printf("Game already started, rejecting login.\n");
                continue;
            }
            login_message_t* message = (login_message_t*)buff;
            printf("[Login] Welcome, <%s>\n", message->name);

            beloved_t beloved;
            beloved.addr = addr;
            strcpy(beloved.name, message->name);
            beloved.logged = 1;
            beloved.pebbles = 10;
            
            pthread_mutex_lock(&game.mutex);
            add_beloved(beloved, &game);
            pthread_mutex_unlock(&game.mutex);
        }
        else {
            pthread_mutex_lock(&game.mutex);
            int logged = is_logged_in(&game, addr);
            int started = game.started;
            pthread_mutex_unlock(&game.mutex);

            if (!logged) {
                printf("Third wheel or non-player message rejected.\n");
                continue;
            }
            if (!started) {
                printf("Game has not started yet.\n");
                continue;
            }

            if (buff[0] == 'c') {
                cast_message_t* message = (cast_message_t*)buff;
                message->spell = ntohs(message->spell);
                message->X = ntohs(message->X);
                message->Y = ntohs(message->Y);

                if (message->spell >= SPELL_TYPES || message->X >= BOARD_SIZE || message->Y >= BOARD_SIZE) {
                    printf("Error in the arguments, value out of range\n");
                    continue;
                }

                pthread_mutex_lock(&game.mutex);
                message->padding = get_beloved_index(&game, addr);
                pthread_mutex_unlock(&game.mutex);

                queue_push(&game.queue, message);
            }
            else if (buff[0] == 'q') {
                pthread_mutex_lock(&game.mutex);
                int p_idx = get_beloved_index(&game, addr);
                beloved_t* loser = (p_idx == 0) ? &game.Misha : &game.Zosia;
                beloved_t* winner = (p_idx == 0) ? &game.Zosia : &game.Misha;

                printf("[Quit] <%s> quit. Goodbye!\n", loser->name);
                printf("-= Congratulations, <%s>, you win! =-\n", winner->name);

                char w = 'w', l = 'l';
                sendto(game.server_fd, &w, 1, 0, (struct sockaddr*)&winner->addr, sizeof(winner->addr));
                sendto(game.server_fd, &l, 1, 0, (struct sockaddr*)&loser->addr, sizeof(loser->addr));

                game.game_over = 1;
                pthread_mutex_unlock(&game.mutex);
                break;
            }
            else {
                printf("Incorrect message type\n");
            }
        }
    }

    //Waking workers
    for (int i = 0; i < THREAD_COUNT; i++) {
        sem_post(&game.queue.sem);
    }

    for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);
    pthread_join(judge, NULL);

    queue_destroy(&game.queue);
    pthread_mutex_destroy(&game.mutex);
}

void usage(char* name) {
    printf("%s <in_port>\n", name);
    printf("  in_port - port that accepts messages\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        usage(argv[0]);
    }

    uint16_t port = atoi(argv[1]);

    
    int fd = bind_inet_socket(port, SOCK_DGRAM, 10);

    doServer(fd);

    if (close(fd) < 0)
        perror("close");

    return 0;
}