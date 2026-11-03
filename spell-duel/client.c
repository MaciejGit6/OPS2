/* Interactive client for the Spell Duel server.
 *
 * One thread, one loop: poll() watches stdin and the UDP socket at the
 * same time, so typed commands and server messages are handled as soon as
 * either one is ready.
 *
 *   ./spell-duel-client <host> <port> <name>
 */
#include "../common/common.h"
#include <netdb.h>
#include <poll.h>

#define MAX_NAME_LENGTH 14
#define BOARD_SIZE 8
#define DIVINATION_SIDE 5
#define MAX_DATAGRAM 64
#define MAX_LINE 64

/* Same wire layout as in server.c: packed, integers in network byte order. */
typedef struct __attribute__((__packed__)) cast_message_t
{
    char type;
    char padding;
    uint16_t spell;
    uint16_t x;
    uint16_t y;
} cast_message_t;

enum spell
{
    DIVINATION = 0,
    SUMMON_ELEMENTAL = 1,
    FIREBALL = 2
};

void usage(const char* name)
{
    fprintf(stderr, "USAGE: %s <host> <port> <name>\n", name);
    exit(EXIT_FAILURE);
}

void print_help(void)
{
    printf("Commands:\n"
           "  d <x> <y>   Divination        (1 pebble)  look at the 5x5 area around x,y\n"
           "  s <x> <y>   Summon Elemental  (3 pebbles) claim cell x,y\n"
           "  f <x> <y>   Fireball          (4 pebbles) clear the 3x3 area around x,y\n"
           "  q           quit (forfeits the game)\n"
           "  h           this help\n");
}

/* Resolve host:port and connect() a UDP socket to it.
 * A connected UDP socket can use send()/recv(), and the kernel drops any
 * datagram that doesn't come from the server. */
int connect_udp(const char* host, const char* port)
{
    struct addrinfo hints;
    struct addrinfo* res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        exit(EXIT_FAILURE);
    }

    int fd = make_socket(AF_INET, SOCK_DGRAM);
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
        ERR("connect");
    freeaddrinfo(res);
    return fd;
}

void send_login(int fd, const char* name)
{
    size_t len = strlen(name);
    if (len == 0 || len > MAX_NAME_LENGTH)
    {
        fprintf(stderr, "name must be 1-%d characters\n", MAX_NAME_LENGTH);
        exit(EXIT_FAILURE);
    }

    char buf[2 + MAX_NAME_LENGTH];
    buf[0] = 'l';
    buf[1] = '\0';
    memcpy(buf + 2, name, len);
    if (TEMP_FAILURE_RETRY(send(fd, buf, 2 + len, 0)) < 0)
        ERR("send");
}

void send_cast(int fd, enum spell spell, uint16_t x, uint16_t y)
{
    cast_message_t msg;
    msg.type = 'c';
    msg.padding = 0;
    msg.spell = htons(spell);
    msg.x = htons(x);
    msg.y = htons(y);
    if (TEMP_FAILURE_RETRY(send(fd, &msg, sizeof(msg), 0)) < 0)
        ERR("send");
}

void send_quit(int fd)
{
    char q = 'q';
    if (TEMP_FAILURE_RETRY(send(fd, &q, 1, 0)) < 0)
        ERR("send");
}

/* Handle one line typed by the user. Returns false when the user quits. */
bool handle_command(int fd, char* line)
{
    char cmd;
    unsigned x, y;
    int fields = sscanf(line, " %c %u %u", &cmd, &x, &y);
    if (fields < 1)
        return true;

    switch (cmd)
    {
        case 'q':
            send_quit(fd);
            return false;
        case 'h':
            print_help();
            return true;
        case 'd':
        case 's':
        case 'f':
            if (fields != 3)
            {
                printf("usage: %c <x> <y>\n", cmd);
                return true;
            }
            /* The server checks this too, but silently drops bad casts. */
            if (x >= BOARD_SIZE || y >= BOARD_SIZE)
            {
                printf("x and y must be between 0 and %d\n", BOARD_SIZE - 1);
                return true;
            }
            send_cast(fd, cmd == 'd' ? DIVINATION : cmd == 's' ? SUMMON_ELEMENTAL : FIREBALL, x, y);
            return true;
        default:
            printf("unknown command '%c', type h for help\n", cmd);
            return true;
    }
}

void print_divination(const uint16_t* cells)
{
    static const char symbol[] = {'.', 'M', 'E', '#'}; /* empty, mine, enemy, off-board */
    printf("Divination:\n");
    for (int row = 0; row < DIVINATION_SIDE; row++)
    {
        printf("  ");
        for (int col = 0; col < DIVINATION_SIDE; col++)
        {
            uint16_t v = ntohs(cells[row * DIVINATION_SIDE + col]);
            putchar(v < sizeof(symbol) ? symbol[v] : '?');
            putchar(' ');
        }
        putchar('\n');
    }
}

/* Handle one datagram from the server. The message type is given by its
 * length. Returns false once the game is over. */
bool handle_datagram(int fd)
{
    char buf[MAX_DATAGRAM];
    ssize_t n = TEMP_FAILURE_RETRY(recv(fd, buf, sizeof(buf), 0));
    if (n < 0)
    {
        /* On a connected UDP socket an ICMP "port unreachable" comes back as
         * ECONNREFUSED: nobody is listening on that port. */
        if (errno == ECONNREFUSED)
        {
            fprintf(stderr, "server is not running\n");
            exit(EXIT_FAILURE);
        }
        ERR("recv");
    }

    if (n == 1)
    {
        printf(buf[0] == 'w' ? "*** You win! ***\n" : "*** You lose. ***\n");
        return false;
    }
    if (n == sizeof(uint16_t))
    {
        uint16_t pebbles;
        memcpy(&pebbles, buf, sizeof(pebbles)); /* memcpy: buf may be misaligned */
        printf("Pouch: %u pebbles\n", ntohs(pebbles));
        return true;
    }
    if (n == DIVINATION_SIDE * DIVINATION_SIDE * sizeof(uint16_t))
    {
        uint16_t cells[DIVINATION_SIDE * DIVINATION_SIDE];
        memcpy(cells, buf, sizeof(cells));
        print_divination(cells);
        return true;
    }

    printf("unexpected %zd-byte message from server\n", n);
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 4)
        usage(argv[0]);
    parse_port(argv[2]); /* validate only; getaddrinfo takes the string */

    int fd = connect_udp(argv[1], argv[2]);
    send_login(fd, argv[3]);
    printf("Logged in as %s. The game starts when the second player joins.\n", argv[3]);
    print_help();

    struct pollfd fds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = fd, .events = POLLIN},
    };

    bool playing = true;
    while (playing)
    {
        if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0)
            ERR("poll");

        if (fds[1].revents & (POLLIN | POLLERR))
            playing = handle_datagram(fd);

        if (playing && (fds[0].revents & (POLLIN | POLLHUP)))
        {
            char line[MAX_LINE];
            if (fgets(line, sizeof(line), stdin) == NULL)
            {
                /* stdin closed (Ctrl+D): treat as quitting */
                send_quit(fd);
                playing = false;
            }
            else
                playing = handle_command(fd, line);
        }
    }

    if (TEMP_FAILURE_RETRY(close(fd)) < 0)
        ERR("close");
    return EXIT_SUCCESS;
}
