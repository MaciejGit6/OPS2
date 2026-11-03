#include "../common/common.h"
#include <sys/wait.h>

#define MSG_MAX 64
#define RUN_MS 3000

typedef struct
{
    pthread_mutex_t mutex;
    unsigned stop : 1;
} shared_t;

void usage(char* name)
{
    printf("%s <address> <port>\n", name);
    printf("  address - server address\n");
    printf("  port    - server port\n");
    exit(EXIT_FAILURE);
}

long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

shared_t* shared_init(void)
{
    shared_t* shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED)
        ERR("mmap");

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        ERR("pthread_mutexattr_init");
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
        ERR("pthread_mutexattr_setpshared");
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
        ERR("pthread_mutexattr_setrobust");
    if (pthread_mutex_init(&shm->mutex, &attr) != 0)
        ERR("pthread_mutex_init");
    pthread_mutexattr_destroy(&attr);

    shm->stop = 0;
    return shm;
}



void lock_robust(pthread_mutex_t* m)
{
    int err = pthread_mutex_lock(m);
    if (err == EOWNERDEAD)
    {
        if (pthread_mutex_consistent(m) != 0)
            ERR("pthread_mutex_consistent");
    }
    else if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_lock");
    }
}

void unlock(pthread_mutex_t* m)
{
    int err = pthread_mutex_unlock(m);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_unlock");
    }
}

void slave_work(shared_t* shm, int id)
{
    while (1)
    {
        lock_robust(&shm->mutex);
        unsigned stop = shm->stop;
        unlock(&shm->mutex);

        if (stop)
            break;

        printf("[slave %d] I am not yet implemented\n", id);
        fflush(stdout);
        ms_sleep(500);
    }
}

void recv_loop(int sockfd)
{
    struct timeval tv = {0, 200000};
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
        ERR("setsockopt");

    char buf[MSG_MAX + 1];
    struct sockaddr_in peer;
    socklen_t plen;
    long deadline = now_ms() + RUN_MS;

    while (now_ms() < deadline)
    {
        plen = sizeof(peer);
        ssize_t n = recvfrom(sockfd, buf, MSG_MAX, 0, (struct sockaddr*)&peer, &plen);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            ERR("recvfrom");
        }
        buf[n] = '\0';
        printf("[main] %zd bytes from %s:%hu: %s\n", n, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), buf);
        fflush(stdout);
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        usage(argv[0]);

    uint16_t port = parse_port(argv[2]);
    int sockfd = bind_inet_socket(port, SOCK_DGRAM, 0);

    shared_t* shm = shared_init();

    for (int i = 0; i < 10; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");
        if (pid == 0)
        {
            slave_work(shm, i);
            if (TEMP_FAILURE_RETRY(close(sockfd)) < 0)
                ERR("close");
            exit(EXIT_SUCCESS);
        }
    }

    recv_loop(sockfd);

    lock_robust(&shm->mutex);
    shm->stop = 1;
    unlock(&shm->mutex);

    while (wait(NULL) > 0)
        ;

    if (pthread_mutex_destroy(&shm->mutex) != 0)
        ERR("pthread_mutex_destroy");
    if (munmap(shm, sizeof(*shm)) < 0)
        ERR("munmap");
    if (TEMP_FAILURE_RETRY(close(sockfd)) < 0)
        ERR("close");

    return EXIT_SUCCESS;
}