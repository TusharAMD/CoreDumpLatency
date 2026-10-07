#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

int main(int argc, char* argv[]) {
    char* exe = (argc > 1) ? argv[1] : "unknown";
    char* pid = (argc > 2) ? argv[2] : "0";

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/tmp/crash_dumps/core.%s.%s.pipe", exe, pid);

    int out_fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) return 1;

    char buffer[65536];
    ssize_t bytes;
    while ((bytes = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        write(out_fd, buffer, bytes);
    }
    close(out_fd);
    return 0;
}
