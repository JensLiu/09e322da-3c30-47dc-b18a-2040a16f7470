#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{

    if (argc < 2) {
        fprintf(2, "usage: xargs [exec args ...]");
    }

    char buf[512], *p;
    char *arglist[MAXARG];
    char *whitespace = " \t\r\n\v";
    int i;
    for (i = 0; i < argc - 1; i++) {
        arglist[i] = argv[i + 1];
    }

    while (1) {

        i = argc - 1;

        gets(buf, 512);  // read each line
        if (strlen(buf) == 0) { // exit the loop
            break;
        }

        char *ps = buf, *pe, *newarg;
        
        while (1) {  // split into tokens by whitespaces
            
            // ignore leading whitespaces
            while (*ps != 0 && strchr(whitespace, *ps))
                ps++;
            if (*ps == 0)
                break;

            // find the end of the token
            pe = ps;
            while (*pe != 0 && !strchr(whitespace, *pe))
                pe++;
            
            // allocate string
            newarg = malloc((pe - ps + 1) * sizeof(char));
            for (int j = 0; j < pe - ps; j++) {
                newarg[j] = ps[j];
            }
            newarg[pe - ps] = 0;
            

            // add to arglist
            arglist[i++] = newarg;

            // move to next
            ps = pe;
        }
        
        arglist[i] = 0;

        if (fork() == 0) {
            // child
            exec(arglist[0], arglist);
            fprintf(2, "xargs: internal error");
        }

        wait(0);                                // wait for it to terminate
        for (int j = argc - 1; j < i; j++) {    // free additionally allocated spaces
            free(arglist[j]);
        }
        
    }
    exit(0);
}