
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

// TODO:
// 1. Sort output alphabetically.
// 2. Add the "total" line for -l.
// 3. Support -d because you already tested it.
// 4. Add -A to show hidden files but skip "." and "..".
// 5. Make Windows permissions visually honest, maybe rwx------ or rw-/--- is fine, but document
// that it is approximate.

// On Linux/POSIX, this asks the headers to expose POSIX functions like lstat().
// This must be defined before including system headers.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
// Windows-specific API. We use this only for file attributes like
// FILE_ATTRIBUTE_REPARSE_POINT, which can help us detect symlinks/junctions.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
// These are POSIX/Linux headers.
// They do not exist in normal native Windows C environments.
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#endif

// A simple max path buffer for this learning project.
// Real production code should avoid fixed-size path buffers where possible.
#define PATH_BUFFER_SIZE 4096

// whether to show or not the "." files in the dir
int show_all = 0;
// whether to print the long format of the files
int long_format = 0;

// Join "dir" and "name" into one full path.
//
// On Linux, paths usually use "/".
// On Windows, paths usually use "\", although many Windows APIs also accept "/".
// We keep this explicit because you are learning portability.
void join_path(char *out, size_t out_size, const char *dir, const char *name) {
#ifdef _WIN32
    const char *separator = "\\";
#else
    const char *separator = "/";
#endif

    size_t dir_len = strlen(dir);

    // If dir already ends with "/" or "\", do not add another separator.
    int needs_separator = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';

    snprintf(out, out_size, "%s%s%s", dir, needs_separator ? separator : "", name);
}

// Portable wrapper around stat/lstat.
//
// Linux:
//   lstat() gives information about the link itself if the path is a symlink.
// Windows:
//   stat() is available, but Windows does not expose the same POSIX symlink model.
int stat_file(const char *path, struct stat *st) {
#ifdef _WIN32
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

// Determine the first character of ls -l output:
//
// '-' regular file
// 'd' directory
// 'l' symbolic link
// 'b' block device
// 'c' character device
// 'p' FIFO / pipe
// 's' socket
//
// Not every operating system supports every file type.
// That is why we use #ifdef around some macros.
char file_type_char(const char *path, mode_t mode) {
#ifdef _WIN32
    // On Windows, symlinks and junctions are represented as reparse points.
    // FILE_ATTRIBUTE_REPARSE_POINT includes symbolic links, but also other
    // reparse-point types such as junctions. For this learning project, we
    // display any reparse point as 'l'.
    DWORD attrs = GetFileAttributesA(path);

    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            return 'l';
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            return 'd';
        }
    }
#else
    // On Linux/POSIX, lstat() lets S_ISLNK() detect a symlink without following it.
    (void)path;

#ifdef S_ISLNK
    if (S_ISLNK(mode)) {
        return 'l';
    }
#endif

#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        return 's';
    }
#endif
#endif

#ifdef S_ISDIR
    if (S_ISDIR(mode)) {
        return 'd';
    }
#endif

#ifdef S_ISBLK
    if (S_ISBLK(mode)) {
        return 'b';
    }
#endif

#ifdef S_ISCHR
    if (S_ISCHR(mode)) {
        return 'c';
    }
#endif

#ifdef S_ISFIFO
    if (S_ISFIFO(mode)) {
        return 'p';
    }
#endif

    return '-';
}

// take a mode and a string, then do checks
void mode_string(const char *path, mode_t mode, char *str) {
    // determine first character
    // is this mode a dir, symlink, regular file, etc.
    str[0] = file_type_char(path, mode);

    // build the permissions string
    // we use the bitwise and & operator
    // It only keeps bits that are set on both sides and returns either non 0 for true 0 for false
    str[1] = (mode & S_IRUSR) ? 'r' : '-';
    str[2] = (mode & S_IWUSR) ? 'w' : '-';
    str[3] = (mode & S_IXUSR) ? 'x' : '-';

#ifdef _WIN32
    // Windows does not have the same owner/group/other permission model as Unix.
    // MinGW may expose some permission bits, but they do not mean exactly the same
    // thing as Linux permissions.
    //
    // To avoid pretending Windows has full Unix permissions, we only show user bits
    // and leave group/other as "-".
    str[4] = '-';
    str[5] = '-';
    str[6] = '-';
    str[7] = '-';
    str[8] = '-';
    str[9] = '-';
#else
    str[4] = (mode & S_IRGRP) ? 'r' : '-';
    str[5] = (mode & S_IWGRP) ? 'w' : '-';
    str[6] = (mode & S_IXGRP) ? 'x' : '-';
    str[7] = (mode & S_IROTH) ? 'r' : '-';
    str[8] = (mode & S_IWOTH) ? 'w' : '-';
    str[9] = (mode & S_IXOTH) ? 'x' : '-';
#endif

    // null terminator, null byte, it tells to stop printing
    str[10] = '\0';
}

// Convert uid/gid into readable user/group strings.
//
// Linux:
//   Use getpwuid() and getgrgid().
//
// Windows:
//   The normal Windows stat fields st_uid/st_gid are not useful.
//   So we print "-" for now.
void owner_group_strings(const struct stat *st, char *user, size_t user_size, char *group,
                         size_t group_size) {
#ifdef _WIN32
    (void)st;

    snprintf(user, user_size, "-");
    snprintf(group, group_size, "-");
#else
    struct passwd *pw = getpwuid(st->st_uid);

    // Important:
    // getgrgid() must use st_gid, not st_uid.
    struct group *gr = getgrgid(st->st_gid);

    snprintf(user, user_size, "%s", pw ? pw->pw_name : "?");
    snprintf(group, group_size, "%s", gr ? gr->gr_name : "?");
#endif
}

// takes a directory string and a name strings
void print_long(const char *dir, const char *name) {
    // a buffer to hold the max path size we are allowing in this learning project
    char fullpath[PATH_BUFFER_SIZE];

    // used to write formatted data string into a sized character buffer
    // helps us ensure the path fits in a buffer of the max path size
    join_path(fullpath, sizeof(fullpath), dir, name);

    // initialize an empty struct
    struct stat st;

    // on linux this uses lstat
    // on windows this uses stat
    if (stat_file(fullpath, &st) < 0) {
        perror(fullpath);
        return;
    }

    char modes[11];
    mode_string(fullpath, st.st_mode, modes);

    char user[64];
    char group[64];
    owner_group_strings(&st, user, sizeof(user), group, sizeof(group));

    char timebuf[64];

    // st_mtime is portable enough for this case.
    // On Linux it maps to the modification time.
    // On Windows CRT it is also the modification time.
    time_t modified_time = st.st_mtime;
    struct tm *tm = localtime(&modified_time);

    if (tm == NULL) {
        snprintf(timebuf, sizeof(timebuf), "?");
    } else {
        strftime(timebuf, sizeof(timebuf), "%b %e %H:%M", tm);
    }

    printf("%s %lu %-8s %-8s %8ld %s %s\n", modes, (unsigned long)st.st_nlink, user, group,
           (long)st.st_size, timebuf, name);
}

// Print help message for invalid usage.
void print_usage(const char *program_name) {
    fprintf(stderr, "usage: %s [-al] [path]\n", program_name);
}

// Simple option parser.
//
// You were using getopt(), which is standard POSIX and works well on Linux.
// But getopt() is not a safe assumption for native Windows portability.
//
// For this small project, manual parsing is simpler and makes the program
// easier to compile on both Windows and Linux.
const char *parse_args(int argc, char *argv[]) {
    const char *path = ".";

    // argv[0] is the program name, so real arguments start at index 1.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else if (strcmp(argv[i], "-l") == 0) {
            long_format = 1;
        } else if (strcmp(argv[i], "-al") == 0 || strcmp(argv[i], "-la") == 0) {
            show_all = 1;
            long_format = 1;
        } else if (argv[i][0] == '-') {
            print_usage(argv[0]);
            return NULL;
        } else {
            // First non-option argument is the path.
            path = argv[i];
        }
    }

    return path;
}

// to take parameters we need to change it from void to int argc, char *argv[]
// int argc = means the argument count, how many words when they ran the program
// char *argv[] = a pointer to a pointer of an array of strings, in C strings
// are an array of chars
int main(int argc, char *argv[]) {
    const char *path = parse_args(argc, argv);

    if (path == NULL) {
        return 1;
    }

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

    // count the number of items inside the dir
    size_t count = 0;
    // Initial size
    size_t capacity = 64;

    // 1. Initial list allocation, Initialize a pointer-to-pointer (char **names) to hold your list
    // of string addresses
    char **names_output_buffer = malloc(capacity * sizeof(char *));

    if (names_output_buffer == NULL) {
        // Handle allocation failure
        perror("malloc");
        return 1;
    }

    // while there are entries, print their names
    while ((entry = readdir(dir)) != NULL) {
        // check if the first element of the name is a dot, skipping it. Here we use
        // single '' that creates a normal char, instead of "" which is a pointer
        // char
        if (!show_all && entry->d_name[0] == '.') {
            // continue means, exit the iteration
            continue;
        }

        // 2. Expand list if full
        if (count >= capacity) {
            // increase capacity
            capacity *= 2;
            // resize array buffer
            char **temp_array = realloc(names_output_buffer, capacity * sizeof(char *));
            if (!temp_array) {
                // Error handling
                printf("realloc");
                break;
            }
            names_output_buffer = temp_array;
        }

        // 3. "Push" name into list (strdup allocates memory for the string copy)
        names_output_buffer[count++] = strdup(entry->d_name);

        if (long_format) {
            print_long(path, entry->d_name);
        } else {

            printf("%s\n", entry->d_name);
        }
    }

    // close the directory, freeing it to other programs
    closedir(dir);

    // 4. Print and cleanup
    printf("Items found:\n");
    for (size_t i = 0; i < count; i++) {
        printf("[%zu] %s\n", i, names_output_buffer[i]);
        free(names_output_buffer[i]); // Free individual string
    }

    // free from memory
    free(names_output_buffer);
    names_output_buffer = NULL; // Good practice to prevent dangling pointers

    // exit main with success
    return 0;
}
