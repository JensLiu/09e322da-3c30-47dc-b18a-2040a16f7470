#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int fd[2];
    int cpid;
    char buf[5];

    if (pipe(fd)) {
        fprintf(2, "internal error");
        exit(1);
    }

    if ((cpid = fork()) == 0) {
        // child
        write(fd[1], "pong", 5);
        read(fd[0], buf, 5);
        fprintf(1, "%d: received %s\n", getpid(), buf);
        close(fd[0]);
        close(fd[1]);
        exit(0);
    } else {
        // parent
        write(fd[1], "ping", 5);
        wait(0);
        read(fd[0], buf, 5);
        fprintf(1, "%d: received %s\n", getpid(), buf);
        close(fd[0]);
        close(fd[1]);
        exit(0);
    }
    exit(0);
}