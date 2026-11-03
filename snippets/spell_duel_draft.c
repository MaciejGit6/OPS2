#include "../common/common.h"

#define SPELL_TYPES 3
const char* spell_names[SPELL_TYPES] = {"Divination", "Summon Elemental", "Fireball"};
#define BOARD_SIZE 8
#define BACKLOG 16

#define MAX_QUEUE 10
#define THREAD_COUNT 3
#define FAMILIAR_DELAY 100

#define MAX_CLIENTS 2
#define MAX_NAME_LENGTH 14

#define MAX_BUFF_LEN 16
#define RECV_BUFF_LEN 64

// typedef struct __attribute__((__packed__)) packed
// {
//     char c1;
//     int i1;
//     char c2;
//     int i2;
// };
// typedef struct not_packed
// {
//     char c1;
//     int i1;
//     char c2;
//     int i2;
// };

typedef struct message_t
{
    char buff[16];
} message_t;

typedef struct
{
    pthread_mutex_t mutex;
    unsigned stop : 1;
} shared_t;

typedef struct __attribute__((__packed__)) cast_message_t
{
    char type;
    char padding;
    uint16_t spell;
    uint16_t X;
    uint16_t Y;

} cast_message_t;

typedef struct __attribute__((__packed__)) quit_message_t
{
    char type;

} quit_message_t;

typedef struct __attribute__((__packed__)) login_message_t
{
    char type;
    char padding;
    char name[MAX_NAME_LENGTH + 1];

} login_message_t;

typedef struct
{
    cast_message_t buff[MAX_QUEUE];
    pthread_mutex_t mutex;
    sem_t sem;
    int head;
    int tail;
    int count;

} queue_t;

typedef struct
{
    struct sockaddr_in addr;
    char name[MAX_NAME_LENGTH + 1];
    int pebbles;
    int logged;
} beloved_t;

typedef struct
{
    beloved_t Misha;
    beloved_t Zosia;
    int started;
    int stop;
    queue_t queue;
    pthread_mutex_t mutex;
} gamestate_t;

void queue_init(queue_t* queue)
{
    pthread_mutex_init(&queue->mutex, NULL);
    sem_init(&queue->sem, 0, 0);
    queue->head = queue->tail = queue->count = 0;
}
void queue_destroy(queue_t* queue)
{
    pthread_mutex_destroy(&queue->mutex);
    sem_destroy(&queue->sem);
}
void queue_push(queue_t* queue, cast_message_t* message)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->count < MAX_QUEUE)
    {
        memcpy(&queue->buff[queue->tail], message, sizeof(cast_message_t));
        queue->tail = (queue->tail + 1) % MAX_QUEUE;
        queue->count++;
        sem_post(&queue->sem);
    }
    pthread_mutex_unlock(&queue->mutex);
}

cast_message_t queue_pop(queue_t* queue)
{
    sem_wait(&queue->sem);

    pthread_mutex_lock(&queue->mutex);
    cast_message_t message = queue->buff[queue->head];
    queue->head = (queue->head + 1) % MAX_QUEUE;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);

    return message;
}

void gamestate_init(gamestate_t* game)
{
    game->Misha.name[0] = 0;
    game->Zosia.name[0] = 0;
    game->Misha.pebbles = 10;
    game->Zosia.pebbles = 10;
    game->Zosia.logged = 0;
    game->Misha.logged = 0;
    game->started = 0;
    game->stop = 0;
    pthread_mutex_init(&game->mutex, NULL);
    queue_init(&game->queue);
}

void add_beloved(beloved_t beloved, gamestate_t* game)
{
    if ((game->Misha.logged &&
         !memcmp(&game->Misha.addr, (struct sockaddr_in*)&beloved.addr, sizeof(struct sockaddr_in))) ||
        (game->Zosia.logged &&
         !memcmp(&game->Zosia.addr, (struct sockaddr_in*)&beloved.addr, sizeof(struct sockaddr_in))))
    {
        printf("Rejecting the beloved because of self-love\n");
        return;
    }

    if (!game->Misha.logged)
    {
        game->Misha = beloved;
        printf("Misha joined\n");
    }

    else if (!game->Zosia.logged)
    {
        game->Zosia = beloved;
        game->started = 1;
        printf("Zosia joined\n");
    }

    else
    {
        printf("We like monogamy\n");
    }
}

int is_logged_in(gamestate_t* game, struct sockaddr_in addr)
{
    if (game->Misha.logged && !memcmp(&game->Misha.addr, &addr, sizeof(addr)))
    {
        return 1;
    }
    else if (game->Zosia.logged && !memcmp(&game->Zosia.addr, &addr, sizeof(addr)))
    {
        return 1;
    }
    return 0;
}

int get_beloved_index(gamestate_t* game, struct sockaddr_in addr)
{
    if (game->Misha.logged && !memcmp(&game->Misha.addr, &addr, sizeof(addr)))
    {
        return 0;
    }
    else if (game->Zosia.logged && !memcmp(&game->Zosia.addr, &addr, sizeof(addr)))
    {
        return 1;
    }
    return -1;
}

void usage(char* name)
{
    printf("%s <in_port>\n", name);
    printf("  in_port - port that accepts messages\n");
    exit(EXIT_FAILURE);
}

void launch_threads(pthread_t* tids, int count, void* (*start_routine)(void*), void* args, size_t arg_size)
{
    for (int i = 0; i < count; i++)
    {
        int err = pthread_create(&tids[i], NULL, start_routine, (char*)args + i * arg_size);
        if (err != 0)
        {
            errno = err;
            ERR("pthread_create");
        }
    }
}

void join_threads(pthread_t* tids, int count)
{
    for (int i = 0; i < count; i++)
    {
        int err = pthread_join(tids[i], NULL);
        if (err != 0)
        {
            errno = err;
            ERR("pthread_join");
        }
    }
}

void* worker(void* args)
{
    gamestate_t* game = args;

    while (1)
    {
        cast_message_t message = queue_pop(&game->queue);

        pthread_mutex_lock(&game->mutex);
        if (game->stop)
        {
            pthread_mutex_unlock(&game->mutex);
            break;
        }
        char* name;
        if (message.padding == 0)
        {
            name = game->Misha.name;
        }
        else
        {
            name = game->Zosia.name;
        }
        printf("[Cast] <%s> casts <%s> onto <%hu>,<%hu>\n", name, spell_names[message.spell], message.X, message.Y);
        pthread_mutex_unlock(&game->mutex);

        // for that player implement the logic, some if statements
    }
    return NULL;
}

void doServer(int fd)
{
    pthread_t threads[THREAD_COUNT];

    gamestate_t game;
    gamestate_init(&game);

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        if (pthread_create(&threads[i], NULL, worker, &game) != 0)
            ERR("pthread_create");
    }

    struct sockaddr_in addr;
    socklen_t addr_len;
    char buff[RECV_BUFF_LEN + 1];
    int count_message = 0;
    while (count_message <= 4)
    {
        int message_len;
        addr_len = sizeof(addr);
        memset(buff, 0, sizeof(buff));
        if ((message_len = recvfrom(fd, buff, RECV_BUFF_LEN, 0, (struct sockaddr*)&addr, &addr_len)) < 0)
        {
            perror("recvfrom");
            continue;
        }

        if (message_len < 1)
        {
            printf("message of incorrect length\n");
            continue;
        }

        if (buff[0] == 'l')
        {
            int name_len = message_len - (int)offsetof(login_message_t, name);
            if (name_len < 1)
            {
                printf("message of incorrect length\n");
                continue;
            }
            if (name_len > MAX_NAME_LENGTH)
                name_len = MAX_NAME_LENGTH;

            login_message_t* message;
            message = (login_message_t*)buff;
            count_message++;

            beloved_t beloved;
            memset(&beloved, 0, sizeof(beloved));
            beloved.addr = addr;
            memcpy(beloved.name, message->name, name_len);
            beloved.name[name_len] = '\0';
            printf("[Login] Welcome, <%s>\n", beloved.name);
            beloved.logged = 1;
            beloved.pebbles = 10;
            pthread_mutex_lock(&game.mutex);
            add_beloved(beloved, &game);
            pthread_mutex_unlock(&game.mutex);
        }
        else
        {
            pthread_mutex_lock(&game.mutex);
            if (!is_logged_in(&game, addr))
            {
                printf("Third wheel\n");
                pthread_mutex_unlock(&game.mutex);
                continue;
            }
            pthread_mutex_unlock(&game.mutex);
            if (buff[0] == 'c')
            {
                if (message_len < (int)sizeof(cast_message_t))
                {
                    printf("message of incorrect length\n");
                    continue;
                }
                cast_message_t* message;
                message = (cast_message_t*)buff;

                message->spell = ntohs(message->spell);
                message->X = ntohs(message->X);
                message->Y = ntohs(message->Y);
                pthread_mutex_lock(&game.mutex);
                message->padding = get_beloved_index(&game, addr);
                pthread_mutex_unlock(&game.mutex);

                if (message->spell >= SPELL_TYPES || message->X >= BOARD_SIZE || message->Y >= BOARD_SIZE)
                {
                    printf("Error in the arguments, value out of range\n");
                    continue;
                }

                count_message++;
                queue_push(&game.queue, message);
            }

            else if (buff[0] == 'q')
            {
                break;
            }

            else
            {
                printf("Incorrect message type\n");
            }
        }
    }
    pthread_mutex_lock(&game.mutex);
    game.stop = 1;
    pthread_mutex_unlock(&game.mutex);
    for (int i = 0; i < THREAD_COUNT; i++)
        sem_post(&game.queue.sem);

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }
    queue_destroy(&game.queue);
    pthread_mutex_destroy(&game.mutex);
}

int main(int argc, char** argv)
{
    // printf("sizeof(struct packed) == %d\n", sizeof(struct packed));
    // printf("sizeof(struct not_packed) == %d\n", sizeof(struct not_packed));

    if (argc != 2)
    {
        usage(argv[0]);
    }

    uint16_t port = atoi(argv[1]);

    int fd = bind_inet_socket(port, SOCK_DGRAM, 10);

    doServer(fd);

    if (close(fd) < 0)
        ERR("close");

    return 0;
}