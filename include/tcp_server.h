#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>

#define MAX_EVENTS  10
#define MAX_CLIENTS 256
#define BUF_SIZE    128

void buf_init(char *read_buf,char *write_buf);
int setnonblocking(int fd);
void remove_client(int fd,int *clients,int *client_count);

#endif

