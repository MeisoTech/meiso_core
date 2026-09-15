/* Include libc declarations before redirecting calls: fortified recv aliases
 * otherwise bypass a command-line -Drecv=... at optimized Linux builds. */
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>

ssize_t meiso_test_send(int fd, const void *bytes, size_t size, int flags);
ssize_t meiso_test_recv(int fd, void *bytes, size_t size, int flags);

#define send meiso_test_send
#define recv meiso_test_recv
#include "../adapters/posix/composition_media_posix_tcp.c"
