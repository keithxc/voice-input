#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(FILE *stream) {
    fputs("Usage: voice-inputctl [--socket PATH] COMMAND\n"
          "Commands: status, start, stop, toggle, sources, monitor, quit\n", stream);
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (vi_runtime_socket_path(socket_path, sizeof(socket_path)) < 0) return EXIT_FAILURE;
    const char *command = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            if (strlen(argv[++i]) >= sizeof(socket_path)) return EXIT_FAILURE;
            strcpy(socket_path, argv[i]);
        } else if (strcmp(argv[i], "--version") == 0) {
            puts("voice-inputctl " VOICE_INPUT_VERSION);
            return EXIT_SUCCESS;
        } else if (command == NULL) {
            command = argv[i];
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (command == NULL) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    bool monitor = strcmp(command, "monitor") == 0;
    if (!monitor && vi_parse_command(command) == VI_COMMAND_INVALID) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    int fd = connect_socket(socket_path);
    if (fd < 0) {
        fprintf(stderr, "voice-inputctl: cannot connect to %s: %s\n",
                socket_path, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!monitor) {
        char request[48];
        int length = snprintf(request, sizeof(request), "%s\n", command);
        if (send(fd, request, (size_t)length, MSG_NOSIGNAL) != length) {
            perror("voice-inputctl: send");
            close(fd);
            return EXIT_FAILURE;
        }
    }

    FILE *input = fdopen(fd, "r");
    if (input == NULL) {
        close(fd);
        return EXIT_FAILURE;
    }
    /* The sources reply carries every discovered node, so keep the buffer large
       enough that one event stays one fgets() line; a split line would also
       miscount the replies we still have to wait for. */
    static char line[16384];
    int lines_needed = monitor ? -1 : 2;
    while (fgets(line, sizeof(line), input) != NULL) {
        fputs(line, stdout);
        fflush(stdout);
        const size_t length = strlen(line);
        if (length > 0U && line[length - 1U] != '\n') continue;
        if (lines_needed > 0 && --lines_needed == 0) break;
    }
    fclose(input);
    return EXIT_SUCCESS;
}
