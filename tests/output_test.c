#define _POSIX_C_SOURCE 200809L
#include "output.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char root[] = "/tmp/vi-output-XXXXXX";
    assert(mkdtemp(root));
    assert(setenv("XDG_RUNTIME_DIR", root, 1) == 0);
    char directory[128];
    snprintf(directory, sizeof(directory), "%s/voice-input", root);
    assert(mkdir(directory, 0700) == 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/voice-input/fcitx5.sock", root);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(server >= 0);
    assert(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(server, 4) == 0);
    /* A peer which accepts but never acknowledges used to hang forever. */
    assert(vi_output_commit("test") == -1);
    int client = accept(server, NULL, NULL);
    assert(client >= 0);
    close(client);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int peer = accept(server, NULL, NULL);
        unsigned char packet[8];
        if (recv(peer, packet, sizeof(packet), MSG_WAITALL) != 8 ||
            memcmp(packet, "\0\0\0\4test", 8) != 0) _exit(1);
        unsigned char ack = 1;
        if (send(peer, &ack, 1, 0) != 1) _exit(1);
        close(peer);
        _exit(0);
    }
    assert(vi_output_commit("test") == 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(server);
    unlink(address.sun_path);
    rmdir(directory);
    rmdir(root);
    return 0;
}
