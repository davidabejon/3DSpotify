#include <sys/socket.h>
struct addrinfo {
	int ai_family;
	int ai_socktype;
	struct sockaddr *ai_addr;
	socklen_t ai_addrlen;
};
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
int getaddrinfo(const char *host, const char *service,
                const struct addrinfo *hints, struct addrinfo **result);
void freeaddrinfo(struct addrinfo *result);
