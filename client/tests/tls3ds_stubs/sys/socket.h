#ifndef TEST_SOCKET_H
#define TEST_SOCKET_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned socklen_t;
struct sockaddr { unsigned short sa_family; char data[14]; };
struct sockaddr_storage { unsigned short ss_family; char data[26]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xFFFF
#define SO_ERROR 0x1009
#define socket test_socket
#define connect test_connect
#define getsockopt test_getsockopt
#define getpeername test_getpeername
#define send test_send
#define recv test_recv
int socket(int family, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t size);
int getsockopt(int fd, int level, int option, void *value, socklen_t *size);
int getpeername(int fd, struct sockaddr *addr, socklen_t *size);
int send(int fd, const void *buf, size_t len, int flags);
int recv(int fd, void *buf, size_t len, int flags);
#endif
