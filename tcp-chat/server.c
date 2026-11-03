/* Multi-client TCP chat server built on epoll.
 *
 * A single thread serves every client: epoll tells us which sockets are
 * readable, and we only ever read from those, so the loop never blocks
 * on one slow client.
 *
 * Protocol (plain text, one message per line):
 *   - the first line a client sends is its name
 *   - every later line is broadcast to everyone else as "[name] text"
 *   - "/who" lists who is online
 *
 *   ./tcp-chat <port>        then connect with: nc localhost <port>
 */
#include "../common/common.h"
#include <fcntl.h>
#include <sys/epoll.h>

#define BACKLOG 16
#define MAX_CLIENTS 16
#define MAX_EVENTS 16
#define MAX_LINE 256
#define MAX_NAME 32

typedef struct
{
    int fd;              /* -1 when the slot is free */
    char name[MAX_NAME]; /* empty until the client has sent its first line */
    char buf[MAX_LINE];  /* bytes received but not yet ended by '\n' */
    size_t len;
} client_t;

volatile sig_atomic_t do_work = 1;

void sigint_handler(int sig)
{
    (void)sig;
    do_work = 0;
}

void usage(const char* name)
{
    fprintf(stderr, "USAGE: %s <port>\n", name);
    exit(EXIT_FAILURE);
}

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        ERR("fcntl");
}

void epoll_add(int epfd, int fd)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        ERR("epoll_ctl");
}

client_t* find_client(client_t* clients, int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == fd)
            return &clients[i];
    return NULL;
}

/* Write a whole message to one client. If the client has gone away the
 * write fails with EPIPE (SIGPIPE is ignored). The next read on that
 * socket returns 0 and drops it, so there's nothing to do here. */
void send_to(client_t* c, const char* msg)
{
    if (bulk_write(c->fd, (char*)msg, strlen(msg)) < 0 && errno != EPIPE && errno != ECONNRESET)
        ERR("write");
}

/* Send msg to every named client except `from` (which may be NULL). */
void broadcast(client_t* clients, client_t* from, const char* msg)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 && &clients[i] != from && clients[i].name[0])
            send_to(&clients[i], msg);
}

void accept_client(int epfd, int listen_fd, client_t* clients)
{
    int fd = TEMP_FAILURE_RETRY(accept(listen_fd, NULL, NULL));
    if (fd < 0)
    {
        /* The listening socket is non-blocking: if the client gave up
         * between epoll_wait and accept, there's simply nobody to accept. */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
            return;
        ERR("accept");
    }

    client_t* c = find_client(clients, -1);
    if (c == NULL)
    {
        const char* full = "Server is full, try again later.\n";
        if (bulk_write(fd, (char*)full, strlen(full)) < 0 && errno != EPIPE)
            ERR("write");
        if (TEMP_FAILURE_RETRY(close(fd)) < 0)
            ERR("close");
        return;
    }

    c->fd = fd;
    c->name[0] = '\0';
    c->len = 0;
    epoll_add(epfd, fd);
    send_to(c, "Welcome! What's your name?\n");
}

void drop_client(int epfd, client_t* clients, client_t* c)
{
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL) < 0)
        ERR("epoll_ctl");
    if (TEMP_FAILURE_RETRY(close(c->fd)) < 0)
        ERR("close");
    c->fd = -1; /* free the slot before broadcasting so we skip it */

    if (c->name[0])
    {
        char msg[MAX_NAME + 16];
        snprintf(msg, sizeof(msg), "* %s left\n", c->name);
        broadcast(clients, NULL, msg);
        printf("%s disconnected\n", c->name);
    }
}

void send_who(client_t* clients, client_t* to)
{
    char msg[MAX_CLIENTS * (MAX_NAME + 2) + 16] = "Online:";
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 && clients[i].name[0])
        {
            strcat(msg, " ");
            strcat(msg, clients[i].name);
        }
    }
    strcat(msg, "\n");
    send_to(to, msg);
}

void handle_line(client_t* clients, client_t* c, const char* line)
{
    char msg[MAX_NAME + MAX_LINE + 16];

    if (!c->name[0]) /* first line: the name */
    {
        if (!line[0])
        {
            send_to(c, "Name can't be empty, try again:\n");
            return;
        }
        snprintf(c->name, sizeof(c->name), "%s", line);
        snprintf(msg, sizeof(msg), "* %s joined\n", c->name);
        broadcast(clients, c, msg);
        send_to(c, "You're in. Type /who to see who's here.\n");
        printf("%s connected\n", c->name);
        return;
    }

    if (strcmp(line, "/who") == 0)
    {
        send_who(clients, c);
        return;
    }

    snprintf(msg, sizeof(msg), "[%s] %s\n", c->name, line);
    broadcast(clients, c, msg);
}

/* Read what's available from a client and handle every complete line.
 * TCP is a byte stream: one read can return half a line or several lines,
 * so incomplete data stays in c->buf until its '\n' arrives.
 * Returns false if the client disconnected. */
bool handle_readable(client_t* clients, client_t* c)
{
    /* Leave one byte free so an over-long line can still be terminated. */
    ssize_t n = TEMP_FAILURE_RETRY(read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len));
    if (n <= 0) /* 0: client closed the connection, <0: e.g. ECONNRESET */
        return false;
    c->len += n;

    char* start = c->buf;
    char* end = c->buf + c->len;
    char* nl;
    while ((nl = memchr(start, '\n', end - start)) != NULL)
    {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r') /* telnet sends "\r\n" */
            nl[-1] = '\0';
        handle_line(clients, c, start);
        start = nl + 1;
    }

    size_t rest = end - start;
    if (rest == sizeof(c->buf) - 1)
    {
        /* Buffer full and still no newline: treat what we have as a line. */
        c->buf[rest] = '\0';
        handle_line(clients, c, c->buf);
        rest = 0;
    }
    else
        memmove(c->buf, start, rest);
    c->len = rest;
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        usage(argv[0]);
    uint16_t port = parse_port(argv[1]);

    if (sethandler(SIG_IGN, SIGPIPE) < 0)
        ERR("sethandler");
    if (sethandler(sigint_handler, SIGINT) < 0)
        ERR("sethandler");

    /* Keep SIGINT blocked except while we sleep in epoll_pwait. Otherwise
     * a Ctrl+C landing between "while (do_work)" and the call to
     * epoll_wait would be missed until the next event. epoll_pwait swaps
     * in wait_mask (SIGINT unblocked) and waits in one atomic step. */
    sigset_t block, wait_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    if (sigprocmask(SIG_BLOCK, &block, &wait_mask) < 0)
        ERR("sigprocmask");

    int listen_fd = bind_inet_socket(port, SOCK_STREAM, BACKLOG);
    set_nonblocking(listen_fd);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        ERR("epoll_create1");
    epoll_add(epfd, listen_fd);

    client_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    printf("Chat server listening on port %u. Ctrl+C to stop.\n", port);

    struct epoll_event events[MAX_EVENTS];
    while (do_work)
    {
        int n = epoll_pwait(epfd, events, MAX_EVENTS, -1, &wait_mask);
        if (n < 0)
        {
            if (errno == EINTR) /* SIGINT: loop condition sees do_work == 0 */
                continue;
            ERR("epoll_pwait");
        }

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            if (fd == listen_fd)
            {
                accept_client(epfd, listen_fd, clients);
                continue;
            }

            /* EPOLLHUP/EPOLLERR also land here; the read then returns 0 or
             * an error and the client is dropped. */
            client_t* c = find_client(clients, fd);
            if (c != NULL && !handle_readable(clients, c))
                drop_client(epfd, clients, c);
        }
    }

    printf("\nShutting down.\n");
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1)
        {
            send_to(&clients[i], "* Server is shutting down. Bye!\n");
            if (TEMP_FAILURE_RETRY(close(clients[i].fd)) < 0)
                ERR("close");
        }
    }
    if (TEMP_FAILURE_RETRY(close(epfd)) < 0)
        ERR("close");
    if (TEMP_FAILURE_RETRY(close(listen_fd)) < 0)
        ERR("close");
    return EXIT_SUCCESS;
}
