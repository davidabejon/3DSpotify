struct pollfd { int fd; int events; int revents; };
#define POLLIN 1
#define POLLOUT 16
#define POLLERR 8
#define POLLHUP 4
#define POLLNVAL 32
#define poll test_poll
int poll(struct pollfd *fds, unsigned count, int timeout);
