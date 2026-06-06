
// main function that takes no arguments, returns an int. 0 = success
// #include <stdio.h>
// int main(void){
//     printf("hello from world");
//     return 0;
// }

// // to take parameters we need to change it from void to int argc, char
// *argv[]
// // int argc = means the argument count, how many words when they ran the
// program
// // char *argv[] = a pointer to a pointer of an array of strings, in C strings
// are an array of chars int main(int argc, char *argv[]){
//     printf("hello from world \n");
//     // print: take the first argument, which is always the name of the
//     program char *first_arg = argv[1]; if (first_arg == NULL){
//         printf("Warning: no argument passed. \n");
//         return 0;
//     }
//     printf("%s \n", first_arg);
//     return 0;
// }
#include <dirent.h>
#include <process.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#define stat_file(path, st) stat((path), (st))
#else
#define stat_file(path, st) lstat((path), (st))
#endif

// take a mode and a string, then do checks
void mode_string(mode_t mode, char *str) {

    // determine first character
    // is this mode a dir
#ifdef S_ISLNK
    if (S_ISLNK(mode)) {
        str[0] = 'l';
    }
#endif
#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        str[0] = 's';
    }
#endif
    if (S_ISDIR(mode)) {
        str[0] = 'd';
    } else if (S_ISBLK(mode)) {
        str[0] = 'b';
    } else if (S_ISCHR(mode)) {
        str[0] = 'c';
    } else if (S_ISFIFO(mode)) {
        str[0] = 'p';
    } else {
        str[0] = '-';
    }

    // build the permissions string
    // we use the bitwise and & operator
    // It only keeps bits that are set on both sides and returns either non 0 for true 0 for false
    str[1] = (mode & S_IRUSR) ? 'r' : '-';
    str[2] = (mode & S_IWUSR) ? 'w' : '-';
    str[3] = (mode & S_IXUSR) ? 'x' : '-';
    str[4] = (mode & S_IRGRP) ? 'r' : '-';
    str[5] = (mode & S_IWGRP) ? 'w' : '-';
    str[6] = (mode & S_IXGRP) ? 'x' : '-';
    str[7] = (mode & S_IROTH) ? 'r' : '-';
    str[8] = (mode & S_IWOTH) ? 'w' : '-';
    str[9] = (mode & S_IXOTH) ? 'x' : '-';

    // null terminator, null byte, it tells to stop printing
    str[10] = '\0';
}

// takes a directory string and a name strings
void print_long(const char *dir, const char *name) {
    // a buffer to hold the max unix path size
    char fullpath[4096];

    // used to write formatted data string into a sized character buffer
    // helps us ensure the path fits in a buffer of the max path size
    snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, name);

    // initialize an empty struct
    struct stat st;
    // on linux is lstat
    if (stat_file(fullpath, &st) < 0) {
        perror(name);
        return;
    }

    char modes[11];
    mode_string(st.st_mode, modes);

    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_uid);
    const char *user = pw ? pw->pw_name : "?";
    const char *group = gr ? gr->gr_name : "?";

    char timebuf[64];

    struct tm *tm = localtime(&st.st_mtim.tv_sec);
    strftime(timebuf, sizeof(timebuf), "%b %e %H:%M", tm);

    printf("%s %lu %s %ld %s %s\n", modes, (unsigned long)st.st_nlink, user, group,
           (long)st.st_size, timebuf, name);
}

// whether to show or not the "." files in the dir
int show_all = 0;
int long_format = 0;

// to take parameters we need to change it from void to int argc, char *argv[]
// int argc = means the argument count, how many words when they ran the program
// char *argv[] = a pointer to a pointer of an array of strings, in C strings
// are an array of chars
int main(int argc, char *argv[]) {
    // extra arguments passed
    int opt;

    // getopt parses options in the cli for the program and to check for valid
    // options
    while ((opt = getopt(argc, argv, "al")) != -1) {
        switch (opt) {
        case 'a':
            show_all = 1;
            break;
        case 'l':
            long_format = 1;
            break;
        default:
            fprintf(stderr, "usage: %s [-al] [path]\n", argv[0]);
            return 1;
        }
    }

    // print: take the first argument, which is always the name of the program
    // // if the option indez is less than the argument count then use the
    // argument value at that option index, else just return the current path
    const char *path = (optind < argc) ? argv[optind] : ".";

    // DIR is an opaque type, we dont need to know what it is, but the systems
    // knows what it is and knows how to use it. It represents a directory type.
    // opendir allows to check if a directory exits and know its contents
    DIR *dir = opendir(path);
    if (dir == NULL) {
        // perror will check the error and return its human readable prefix with the
        // errmsg
        perror("opendir");
        // 1 = error code
        return 1;
    }

    // create a reference pointer struct to dirent struc
    struct dirent *entry;

    // while there are entries, print their names
    while ((entry = readdir(dir)) != NULL) {
        // check if the first element of the name is a dot, skipping it. Here we use
        // single '' that creates a normal char, instead of "" which is a pointer
        // char
        if (!show_all && entry->d_name[0] == '.') {
            // continue means, exit the iteration
            continue;
        }
        if (long_format) {
            print_long(path, entry->d_name);
        } else {
            printf("%s \n", entry->d_name);
        }
    }

    // close the directory, freeing it to other programs
    closedir(dir);

    return 0;
}
