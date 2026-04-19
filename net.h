#ifndef NET_H
#define NET_H

#include <sys/types.h>

ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);

#endif
