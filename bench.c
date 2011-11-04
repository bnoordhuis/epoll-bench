#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

#ifndef __NR_epoll_ctlv
# if __x86_64__
#  define __NR_epoll_ctlv 312
# elif __i386__
#  define __NR_epoll_ctlv 349
# else
#  error "arch not supported"
# endif
#endif

#define ADDR  INADDR_LOOPBACK
#define PORT  34567

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define SAVE_ERRNO(block) do { int errno_ = errno; { block; } errno = errno_; } while (0)

static int tmfd = -1;
static int epfd = -1;
static int npeers;

static unsigned long bytes_read;
static unsigned long bytes_written;


struct epoll_ctl_event {
  int fd;
  int op;
  struct epoll_event event;
} EPOLL_PACKED;


#if HAVE_EPOLL_CTLV
static int epoll_ctlv(int epfd, struct epoll_ctl_event *events, int nevents) {
  return syscall(__NR_epoll_ctlv, epfd, events, nevents);
}
#endif


static void do_report(void) {
  printf(
    "=====================\n"
    "%lu bytes read\n"
    "%lu bytes written\n",
    bytes_read, bytes_written);

  bytes_read = 0;
  bytes_written = 0;
}


static int do_close(int fd) {
  int r;

  do
    r = close(fd);
  while (r == -1 && errno == EINTR);

  return r;
}


static int add_fd(int fd, int events) {
  struct epoll_event ee = { .data.fd = fd, .events = events | EPOLLET };
  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ee);
}


static int make_sock_fd(unsigned short port) {
  struct sockaddr_in sin;
  int sockfd;

  if ((sockfd = socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0)) == -1)
    return -1;

  memset(&sin, 0, sizeof sin);
  sin.sin_addr.s_addr = htonl(ADDR);
  sin.sin_port = htons(port);
  sin.sin_family = AF_INET;

  if (bind(sockfd, (struct sockaddr *) &sin, sizeof sin) == -1)
    return -1;

  return sockfd;
}


static int make_timer_fd(unsigned int ms) {
  struct itimerspec its;

  if ((tmfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)) == -1)
    return -1;

#define SET_TS(ts, ms) ((ts)->tv_sec = (ms) / 1000, (ts)->tv_nsec = ((ms) % 1000) * 1e6)
  SET_TS(&its.it_interval, ms);
  SET_TS(&its.it_value, ms);

  if (timerfd_settime(tmfd, 0, &its, NULL)) {
    SAVE_ERRNO(do_close(tmfd));
    return -1;
  }

  return tmfd;
}


static int do_recv(int fd) {
  char buf[1024];
  ssize_t r;

  do
    r = read(fd, buf, sizeof buf);
  while (r == -1 && errno == EINTR);

  if (r > 0)
    bytes_read += r;

  return r;
}


static int do_send(int fd, const void *data, size_t size) {
  struct sockaddr_in sin;
  ssize_t r;

  static int port = PORT;
  port = PORT + (((port - PORT) + 1) % npeers);

  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(ADDR);
  sin.sin_port = htons(port);

  do
    r = sendto(fd, data, size, 0, (struct sockaddr *) &sin, sizeof sin);
  while (r == -1 && errno == EINTR);

  if (r > 0)
    bytes_written += r;

  return r;
}


static int do_epoll(void) {
  struct epoll_ctl_event events[1024];
  struct epoll_event out[1024];
  int nevents;
  int i, r, n;
  int flags;
  int fd;

#define UPDATE(fd_, events_) \
  do { \
    events[nevents].fd = fd_; \
    events[nevents].op = EPOLL_CTL_MOD; \
    events[nevents].event.events = (events_) | EPOLLET; \
    events[nevents].event.data.fd = fd_; \
    nevents++; \
  } \
  while (0)

  do
    n = epoll_wait(epfd, out, ARRAY_SIZE(out), -1);
  while (n == -1 && errno == EINTR);

  if (n == -1)
    return -1;

  nevents = 0;

  for (i = 0; i < n; i++) {
    flags = out[i].events;
    fd = out[i].data.fd;

    if (flags & ~(EPOLLIN|EPOLLOUT))
      return -1;

    if (fd == tmfd) {
      char buf[8];
      read(fd, buf, 8);
      do_report();
      continue;
    }

    if (flags & EPOLLIN) {
      if ((r = do_recv(fd)) == -1)
        return -1;
      else
        UPDATE(fd, EPOLLOUT);
    }

    if (flags & EPOLLOUT) {
      const char reply[] = "PING";

      if ((r = do_send(fd, reply, sizeof(reply) - 1)) == -1)
        return -1;
      else
        UPDATE(fd, EPOLLIN);
    }
  }

#if HAVE_EPOLL_CTLV
  n = epoll_ctlv(epfd, events, nevents);
  if (n == -1)
    perror("epoll_ctlv");
  else if (n != nevents)
    fprintf(stderr, "WARN: %d of %d events processed\n", n, nevents);
#else
  for (i = 0; i < nevents; i++) {
    if (epoll_ctl(epfd, events[i].op, events[i].fd, &events[i].event) == -1)
      perror("epoll_ctl");
  }
#endif

  return 0;
}


int main(int argc, char **argv) {
  int i, r;

  (void) argc;

  if (argv[1] == NULL || (npeers = atoi(argv[1])) == 0)
    npeers = 100;

  if ((epfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
    perror("make_epoll_fd");
    exit(1);
  }

  if ((tmfd = make_timer_fd(2000)) == -1) {
    perror("make_timer_fd");
    exit(1);
  }

  if (add_fd(tmfd, EPOLLIN)) {
    perror("add_fd");
    exit(1);
  }

  for (i = 0; i < npeers; i++) {
    int fd;

    if ((fd = make_sock_fd(PORT + i)) == -1)
      perror("make_sock_fd");
    else if (add_fd(fd, EPOLLOUT))
      perror("add_fd");
  }

  while ((r = do_epoll()) == 0)
    /* nop */ ;

  if (r == -1) {
    perror("do_epoll");
    exit(1);
  }

  return 0;
}
