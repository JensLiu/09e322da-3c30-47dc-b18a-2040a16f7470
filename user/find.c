#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

// borrow ftmname from ls.c
char*
filename(char *path)
{
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;
  return p;
}

void find(char *, char *);

int
main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(2, "find <dir> <name>\n");
        exit(0);
    }
    find(argv[1], argv[2]);
    exit(0);
}


void
find(char *path, char *target)
{
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if((fd = open(path, O_RDONLY)) < 0){
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    if(fstat(fd, &st) < 0){
        fprintf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch(st.type) {
        case T_FILE: {
            if (!strcmp(filename(path), target))
                fprintf(1, "%s\n", path);
            break;
        }
        case T_DIR: {
            if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
                printf("find: path too long\n");
                break;
            }
            
            // construct new path: <old-path> /
            strcpy(buf, path);
            p = buf+strlen(buf);
            *p++ = '/';

            // Directory is a file containing a sequence of dirent structures.
            while(read(fd, &de, sizeof(de)) == sizeof(de)){
                if (de.inum == 0)
                    continue;
                // do not recurse into "." and ".."
                if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
                    continue;
                
                // construct new path: <old-path> / <name>
                memmove(p, de.name, DIRSIZ);
                    p[DIRSIZ] = 0;
                    find(buf, target);
                }
            break;
        }

    }
    close(fd);
}

// void
// find(char *path, char *target)
// {
//     fprintf(1, "find path:%s (filename: %s) target: %s\n", path, filename(path), target);
//     char buf[512], *p;
//     int fd;
//     struct dirent de;
//     struct stat st;

//     // open file and get file descripter
//     if (fd = open(path, O_RDONLY) < 0) {
//         fprintf(1, "find: cannot open %s\n", path);
//         return;
//     }

//     // get file status
//     if (fstat(fd, &st) < 0) {
//         fprintf(2, "ls: cannot stat %s\n", path);
//         close(fd);
//         return;
//     }


//     switch(st.type) {
//         case T_DEVICE:
//         case T_FILE:
//             if (!strcmp(filename(path), target)) {
//                 fprintf(1, "found one");
//                 fprintf(1, path);
//             }
//             break;
//         case T_DIR:
//             fprintf(1, "dir\n"); 
//             if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
//                 printf("find: path too long\n");
//                 break;
//             }
//             strcpy(buf, path);
//             p = buf+strlen(buf);
//             *p++ = '/';
            
//             // Directory is a file containing a sequence of dirent structures.
//             while(read(fd, &de, sizeof(de)) == sizeof(de)){
//                 // do not recurse into "." and ".."
//                 if (de.inum == 0 || strcmp(de.name, ".") || strcmp(de.name, ".."))
//                     continue;
//                 memmove(p, de.name, DIRSIZ);
//                 p[DIRSIZ] = 0;
//                 find(p, target);
//             }
//             break;
//         default:
//             fprintf(2, "find: internal error");
//     }

//     close(fd);
//     return;
// }