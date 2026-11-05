#include <sys/socket.h>
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
	unsigned short sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
};
