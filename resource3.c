#include "l8_common.h"
#include <stddef.h>

#define MAX_NAME_LEN 128
#define BACKLOG 16
#define MAX_BUFF_LENGTH 134
typedef struct __attribute__((__packed__)) packed
{
    uint16_t X;
    uint16_t Y;
    uint16_t P;
    char division_name[MAX_NAME_LEN];
} message_t;

void usage(char* name)
{
    printf("%s <in_port>\n", name);
    printf("  in_port - port that accepts messages\n");
    exit(EXIT_FAILURE);
}

void doServer(int fd)
{   
    char buff[MAX_BUFF_LENGTH+1];
    struct sockaddr_in addr;
    while(1)
    {
        socklen_t addr_len = sizeof(addr);
        int recv_len = recvfrom(fd, buff, MAX_BUFF_LENGTH, 0, (struct sockaddr *)&addr, &addr_len);
        if (recv_len < 0) { perror("recvfrom"); continue; }
        if (recv_len < (int)offsetof(message_t, division_name)) {
            fprintf(stderr, "message too short (%d bytes)\n", recv_len);
            continue;
        }
        message_t* message = (message_t*)buff;
        buff[recv_len]='\0';
        printf("%hu:%hu [ally?] %hu [name]: %s\n", ntohs(message->X), ntohs(message->Y), ntohs(message->P), message->division_name);


        
    }    
}
int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argv[0]);
  }
  uint16_t port = (uint16_t)atoi(argv[1]);
  int fd = bind_inet_socket(port, SOCK_DGRAM, BACKLOG);
  doServer(fd);
  if (close(fd) < 0) {
    ERR("close");
  }
  return 0;
}