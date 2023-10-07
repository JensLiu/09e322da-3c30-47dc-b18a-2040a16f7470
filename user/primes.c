#include "kernel/types.h"
#include "user/user.h"

void doNode(int, int, int);

int
main(int argc, char *argv[])
{
    int fd[2];
    pipe(fd);

    if (fork() == 0) {
        // parent
        close(fd[0]);
        for (int i = 2; i <= 35; i++) {
            write(fd[1], (void *)&i, sizeof(int));
        }
        while (wait(0) != -1)
            ;
        exit(0);
    } else {
        // child
        doNode(2, fd[0], fd[1]);
        exit(0);
    }
}

void
doNode(int curdiv, int infd, int outfd)
{
    close(outfd);   // close output end of the pipe
    fprintf(1, "prime %d\n", curdiv);
    
    int num[1];
    int first = 1;

    int ngbrfd[2] = {-1, -1};

    while (read(infd, (void *)num, sizeof(int)) != 0) {
        if (*num % curdiv == 0) {
            // drop
            continue;
        }
        // cannot be divided
        if (first) {
            first = 0;
            pipe(ngbrfd); // create new pipe
            if (fork() == 0) {
                // child process

                doNode(*num, ngbrfd[0], ngbrfd[1]);
                // should not return
            }
            close(ngbrfd[0]);
        } else {
            // send to the next neighbour
            write(ngbrfd[1], (void *)num, sizeof(int));
        }
    }
    close(ngbrfd[1]);   // close created output fd
    while (wait(0) != -1)
        ;
    exit(0);
}