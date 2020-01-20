#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <netinet/in.h>    /* Internet domain header, for struct sockaddr_in */

struct sockaddr_in *init_server(int port);
int set_up_socket(struct sockaddr_in *self, int num_queue);
int accept_connection(int listenfd);

#endif
