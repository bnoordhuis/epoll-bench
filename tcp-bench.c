#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT  8000

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define SAVE_ERRNO(block) \
	do { 										\
		int errno_ = errno;		\
		{ block; }						\
		errno = errno_;				\
	} while (0)

#define E(block)					\
	do { 										\
		errno = 0; 						\
		{ block; } 						\
		if (errno) 						\
			report_error(#block, errno); \
	} while (0)

typedef struct {
	int events;
	int fd;
} conn_rec;

static int epfd = -1;
static int svfd = -1;
static int tmfd = -1;

static conn_rec *conns;
static conn_rec **client_conns;
static conn_rec **server_conns;
static int n_conns;
static int n_client_conns;
static int n_server_conns;

static unsigned long bytes_read;
static unsigned long bytes_written;


static void do_report(void) {
  printf(
    "=====================\n"
    "%lu bytes read\n"
    "%lu bytes written\n",
    bytes_read, bytes_written);

  bytes_read = 0;
  bytes_written = 0;
}


static void report_error(const char *s, int errorno) {
	fprintf(stderr, "%s: %s (%d)\n", s, strerror(errorno), errorno);
	exit(42);
}


static int make_timer_fd(unsigned int ms) {
  struct itimerspec its;

  if ((tmfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) == -1)
    return -1;

#define SET_TS(ts, ms) ((ts)->tv_sec = (ms) / 1000, (ts)->tv_nsec = ((ms) % 1000) * 1e6)
  SET_TS(&its.it_interval, ms);
  SET_TS(&its.it_value, ms);
  E(timerfd_settime(tmfd, 0, &its, NULL));

  return tmfd;
}


static conn_rec *fd_add(int fd, int events) {
	static int index = 0;
	struct epoll_event e;
	conn_rec *c;

	assert(index < (n_conns * 2 + 2));
	c = &conns[index++];
	c->events = events;
	c->fd = fd;

	e.data.ptr = c;
	e.events = events | EPOLLET;
	E(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e));

	return c;
}


static void fd_mod(conn_rec *c, int events) {
	struct epoll_event e;
	c->events = events;
	e.data.ptr = c;
	e.events = events | EPOLLET;
	E(epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e));
}


static void do_read(int fd) {
	char buf[1024];
	ssize_t r;

  do
    if ((r = read(fd, buf, sizeof buf)) > 0)
      bytes_read += r;
  while (r > 0 || (r == -1 && errno == EINTR));

  if (r == -1 && errno != EAGAIN)
    report_error(__func__, errno);
}



static void do_write(int fd, const void *data, size_t size) {
	ssize_t r;

  do
    if ((r = write(fd, data, size)) > 0)
      bytes_written += r;
  while (r > 0 || (r == -1 && errno == EINTR));

  if (r == -1 && errno != EAGAIN)
    report_error(__func__, errno);
}


int main(int argc, char **argv) {
	struct epoll_event events[1024];
	int pending;
	int i, n, r;

	(void) argc;

	if (argv[1] == NULL || (n_conns = atoi(argv[1])) == 0)
		n_conns = 100;

	/* x2 for both ends of streams, +2 for server and timerfd conn_rec */
	E(conns = calloc(n_conns * 2 + 2, sizeof(*conns)));
	E(client_conns = calloc(n_conns, sizeof(**client_conns)));
	E(server_conns = calloc(n_conns, sizeof(**server_conns)));

	E(epfd = epoll_create1(0));
	E(svfd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0));
	{
		int yes = 1;
		E(setsockopt(svfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes));
	}
	{
		struct sockaddr_in s;
		s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		s.sin_family = AF_INET;
		s.sin_port = htons(PORT);
		E(bind(svfd, (struct sockaddr *) &s, sizeof s));
	}
	E(listen(svfd, 1024));
	fd_add(svfd, EPOLLIN);

	for (pending = i = 0; i < n_conns; i++) {
		struct sockaddr_in s;
		conn_rec *c;
		int fd;

		E(fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0));
		s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		s.sin_family = AF_INET;
		s.sin_port = htons(PORT);

		if (connect(fd, (struct sockaddr *) &s, sizeof s) == 0)
			c = fd_add(fd, EPOLLIN);
		else if (errno != EINPROGRESS)
			report_error("connect", errno);
		else {
			c = fd_add(fd, EPOLLOUT);
			pending++;
		}

		client_conns[n_client_conns++] = c;
	}

	while (pending) {
		E(n = epoll_wait(epfd, events, ARRAY_SIZE(events), -1));

		for (i = 0; i < n; i++) {
			conn_rec *c = events[i].data.ptr;

			if (c->fd == svfd) {
				struct sockaddr_in s;
				socklen_t len;
				int fd;

				len = sizeof s;
				E(fd = accept4(svfd, (struct sockaddr *) &s, &len, SOCK_NONBLOCK));
				server_conns[n_server_conns++] = fd_add(fd, EPOLLIN);
			}
			else {
				socklen_t len;
				int status;

				len = sizeof status;
				E(getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &status, &len));

				if (status)
					report_error("getsockopt(SO_ERROR)", EINVAL);

				fd_mod(c, EPOLLIN);
				pending--;
			}
		}
	}

	//assert(n_client_conns == n_server_conns);

	for (i = 0; i < n_client_conns; i++) {
		conn_rec *c = client_conns[i];
		r = write(c->fd, "PING", 4);
		//assert(r == 4);
		fd_mod(c, EPOLLIN|EPOLLOUT);
		c->events = EPOLLIN;
	}

	for (i = 0; i < n_server_conns; i++) {
		conn_rec *c = server_conns[i];
		do_write(c->fd, "PONG", 4);
		fd_mod(c, EPOLLIN|EPOLLOUT);
		c->events = EPOLLIN;
	}

	fd_add(make_timer_fd(2000), EPOLLIN);

	while (1) {
		E(n = epoll_wait(epfd, events, ARRAY_SIZE(events), -1));

		for (i = 0; i < n; i++) {
			conn_rec *c = events[i].data.ptr;

			if (c->fd == tmfd) {
				do_read(c->fd);
        do_report();
				continue;
			}

			if ((events[i].events & EPOLLIN) & (c->events & EPOLLIN)) {
				do_read(c->fd);
        c->events = EPOLLOUT;
        continue;
			}

			if ((events[i].events & EPOLLOUT) & (c->events & EPOLLOUT)) {
				do_write(c->fd, "PING", 4);
        c->events = EPOLLIN;
        continue;
			}
		}
	}

	return 0;
}
