/* Deployment socket setup only. The exec'd Meiso service owns every protocol byte. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned port(const char *s)
{
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    return *s && !*end && value && value <= 65535 ? (unsigned)value : 0;
}

int main(int argc, char **argv)
{
    if (argc < 7 || argc > 39 || (argc - 7) % 2 ||
        !port(argv[3]) || !port(argv[4]) || !port(argv[6])) {
        fprintf(stderr, "usage: %s SERVICE HOST_IP TCP_PORT UDP_PORT DRM_PATH EDGE_UDP_PORT [SERVICE_OPTION VALUE ...]\n", argv[0]);
        return 2;
    }
    /* Descriptor and role ownership stay with this launcher. */
    for (int i = 7; i < argc; i += 2) {
        if (!strcmp(argv[i], "--role") || !strcmp(argv[i], "--control-fd") ||
            !strcmp(argv[i], "--media-fd") || !strcmp(argv[i], "--drm-device")) return 2;
    }
    if (access(argv[1], X_OK)) { perror("service executable"); return 2; }
    struct sockaddr_in peer = {.sin_family = AF_INET}, local = {0};
    if (inet_pton(AF_INET, argv[2], &peer.sin_addr) != 1) return 2;
    int tcp = socket(AF_INET, SOCK_STREAM, 0), udp = socket(AF_INET, SOCK_DGRAM, 0);
    int result = 1;
    struct timeval timeout = {.tv_sec = 5};
    socklen_t size = sizeof(local);
    char tcp_text[24], udp_text[24];
    if (tcp < 0 || udp < 0 || setsockopt(tcp, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) goto out;
    peer.sin_port = htons((uint16_t)port(argv[3]));
    if (connect(tcp, (struct sockaddr *)&peer, sizeof(peer)) ||
        getsockname(tcp, (struct sockaddr *)&local, &size)) goto out;
    local.sin_port = htons((uint16_t)port(argv[6]));
    if (bind(udp, (struct sockaddr *)&local, sizeof(local))) goto out;
    peer.sin_port = htons((uint16_t)port(argv[4]));
    if (connect(udp, (struct sockaddr *)&peer, sizeof(peer)) ||
        getsockname(udp, (struct sockaddr *)&local, &size)) goto out;
    if (fcntl(tcp, F_SETFL, O_NONBLOCK) || fcntl(udp, F_SETFL, O_NONBLOCK)) goto out;
    printf("UDP_PORT=%u\n", (unsigned)ntohs(local.sin_port));
    if (fflush(stdout)) goto out;
    snprintf(tcp_text, sizeof(tcp_text), "%d", tcp);
    snprintf(udp_text, sizeof(udp_text), "%d", udp);
    char *child[48] = {argv[1], "--role", "edge", "--control-fd", tcp_text,
                       "--media-fd", udp_text, "--drm-device", argv[5]};
    int next = 9;
    for (int i = 7; i < argc; ++i) child[next++] = argv[i];
    child[next] = NULL;
    execv(argv[1], child);
out:
    perror("edge launch");
    if (tcp >= 0) close(tcp);
    if (udp >= 0) close(udp);
    return result;
}
