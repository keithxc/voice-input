#define _POSIX_C_SOURCE 200809L

#include "output.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int output_socket_path(char *buffer, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL || runtime[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    int written = snprintf(buffer, size, "%s/voice-input/fcitx5.sock", runtime);
    if (written < 0 || (size_t)written >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int send_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    while (size > 0) {
        ssize_t sent = send(fd, cursor, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        cursor += sent;
        size -= (size_t)sent;
    }
    return 0;
}

int vi_output_commit(const char *text) {
    if (text == NULL || text[0] == '\0') return 0;
    const size_t length = strlen(text);
    if (length > 65535U) {
        errno = EMSGSIZE;
        return -1;
    }
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (output_socket_path(path, sizeof(path)) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, path);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    const uint32_t network_length = htonl((uint32_t)length);
    unsigned char acknowledgment = 0;
    int result = send_all(fd, &network_length, sizeof(network_length));
    if (result == 0) result = send_all(fd, text, length);
    if (result == 0 && recv(fd, &acknowledgment, 1, MSG_WAITALL) != 1) result = -1;
    close(fd);
    return result == 0 && acknowledgment == 1 ? 0 : -1;
}
