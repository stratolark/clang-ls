// Fast-ish, memory-safe learning version of a small ls-like program.
//
// Build on Linux/POSIX:
//   gcc -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -o lsbtw ls.c
//
// Notes:
// - POSIX path uses fstatat(dirfd(...), name, ..., AT_SYMLINK_NOFOLLOW) for directory entries.
// - Windows support here assumes a C environment that provides <dirent.h>, such as MinGW.
//   Native MSVC does not provide POSIX opendir()/readdir() by default.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#endif

#define PATH_BUFFER_SIZE 4096
#define INITIAL_ENTRY_CAPACITY 64
#define LS_BLOCK_SIZE 1024ULL
#define OUTPUT_BUFFER_SIZE (1u << 20)
#define NAME_FIELD_SIZE 64

#ifndef _WIN32
#define NAME_CACHE_LIMIT 64
#endif

typedef struct Options {
    int show_all;         // -a: include hidden entries, including . and ..
    int almost_all;       // -A: include hidden entries, but skip . and ..
    int long_format;      // -l
    int directory_itself; // -d: show the directory entry itself, not its contents
} Options;

typedef struct DirectoryEntry {
    char *name;
    struct stat st;
    unsigned long long blocks;
    int has_stat;
} DirectoryEntry;

#ifdef _WIN32
typedef struct NameCache {
    int unused;
} NameCache;
#else
typedef struct UserCacheEntry {
    uid_t uid;
    char name[NAME_FIELD_SIZE];
} UserCacheEntry;

typedef struct GroupCacheEntry {
    gid_t gid;
    char name[NAME_FIELD_SIZE];
} GroupCacheEntry;

typedef struct NameCache {
    UserCacheEntry users[NAME_CACHE_LIMIT];
    GroupCacheEntry groups[NAME_CACHE_LIMIT];
    size_t user_count;
    size_t group_count;
} NameCache;
#endif

static void print_usage(const char *program_name) {
    fprintf(stderr, "usage: %s [-aAld] [path]\n", program_name ? program_name : "myls");
}

static int is_dot_or_dotdot(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int should_skip_name(const Options *options, const char *name) {
    if (options->show_all) {
        return 0;
    }

    if (options->almost_all) {
        return is_dot_or_dotdot(name);
    }

    return name[0] == '.';
}

static int parse_args(int argc, char *argv[], Options *options, const char **path) {
    *options = (Options){0};
    *path = ".";

    int path_was_set = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            for (size_t j = 1; arg[j] != '\0'; j++) {
                switch (arg[j]) {
                case 'a':
                    options->show_all = 1;
                    break;
                case 'A':
                    options->almost_all = 1;
                    break;
                case 'l':
                    options->long_format = 1;
                    break;
                case 'd':
                    options->directory_itself = 1;
                    break;
                default:
                    fprintf(stderr, "unknown option: -%c\n", arg[j]);
                    print_usage(argv[0]);
                    return -1;
                }
            }
        } else {
            if (path_was_set) {
                fprintf(stderr, "only one path is supported in this learning version\n");
                print_usage(argv[0]);
                return -1;
            }

            *path = arg;
            path_was_set = 1;
        }
    }

    // If both are present, -a wins over -A.
    if (options->show_all) {
        options->almost_all = 0;
    }

    return 0;
}

static char *copy_string(const char *source) {
    size_t length = strlen(source);

    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }

    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, source, length + 1);
    return copy;
}

static void free_entries(DirectoryEntry *entries, size_t count) {
    if (entries == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
    }

    free(entries);
}

static int grow_entries(DirectoryEntry **entries, size_t *capacity) {
    if (*capacity > SIZE_MAX / 2) {
        errno = ENOMEM;
        return -1;
    }

    size_t new_capacity = *capacity * 2;

    if (new_capacity > SIZE_MAX / sizeof(**entries)) {
        errno = ENOMEM;
        return -1;
    }

    DirectoryEntry *resized = realloc(*entries, new_capacity * sizeof(**entries));
    if (resized == NULL) {
        return -1;
    }

    *entries = resized;
    *capacity = new_capacity;
    return 0;
}

#ifdef _WIN32
static int join_path_checked(char *out, size_t out_size, const char *dir, const char *name) {
    const char *separator = "\\";

    size_t dir_len = strlen(dir);
    int needs_separator = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';

    int written = snprintf(out, out_size, "%s%s%s", dir, needs_separator ? separator : "", name);

    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}
#endif

static int stat_path_no_follow(const char *path, struct stat *st) {
#ifdef _WIN32
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

static int stat_entry_in_open_dir(DIR *dir, const char *dir_path, const char *name,
                                  struct stat *st) {
#ifdef _WIN32
    char fullpath[PATH_BUFFER_SIZE];

    if (join_path_checked(fullpath, sizeof(fullpath), dir_path, name) < 0) {
        return -1;
    }

    return stat(fullpath, st);
#else
    (void)dir_path;

    int dir_fd = dirfd(dir);
    if (dir_fd < 0) {
        return -1;
    }

    return fstatat(dir_fd, name, st, AT_SYMLINK_NOFOLLOW);
#endif
}

#ifdef _WIN32
static unsigned long long round_up_div_ull(unsigned long long value, unsigned long long divisor) {
    if (value == 0) {
        return 0;
    }

    return (value + divisor - 1) / divisor;
}
#endif

static unsigned long long allocated_blocks_1024(const char *path, const struct stat *st) {
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return round_up_div_ull((unsigned long long)st->st_size, LS_BLOCK_SIZE);
    }

    FILE_STANDARD_INFO info;
    BOOL ok = GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info));

    CloseHandle(handle);

    if (!ok) {
        return round_up_div_ull((unsigned long long)st->st_size, LS_BLOCK_SIZE);
    }

    return round_up_div_ull((unsigned long long)info.AllocationSize.QuadPart, LS_BLOCK_SIZE);
#else
    (void)path;

    // On Linux, st_blocks is reported in 512-byte units.
    // Convert to 1024-byte ls-like units.
    return (unsigned long long)((st->st_blocks + 1) / 2);
#endif
}

static char file_type_char(const char *path, mode_t mode) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);

    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return 'l';
        }

        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return 'd';
        }
    }
#else
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

static void mode_string(const char *path, mode_t mode, char str[11]) {
    str[0] = file_type_char(path, mode);

    str[1] = (mode & S_IRUSR) ? 'r' : '-';
    str[2] = (mode & S_IWUSR) ? 'w' : '-';
    str[3] = (mode & S_IXUSR) ? 'x' : '-';

#ifdef _WIN32
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

    str[10] = '\0';
}

static int localtime_safe(const time_t *time_value, struct tm *out) {
#ifdef _WIN32
    return localtime_s(out, time_value) == 0;
#else
    return localtime_r(time_value, out) != NULL;
#endif
}

#ifndef _WIN32
static const char *cached_user_name(NameCache *cache, uid_t uid) {
    for (size_t i = 0; i < cache->user_count; i++) {
        if (cache->users[i].uid == uid) {
            return cache->users[i].name;
        }
    }

    struct passwd *pw = getpwuid(uid);
    const char *name = pw ? pw->pw_name : "?";

    if (cache->user_count < NAME_CACHE_LIMIT) {
        UserCacheEntry *slot = &cache->users[cache->user_count++];
        slot->uid = uid;
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        return slot->name;
    }

    return name;
}

static const char *cached_group_name(NameCache *cache, gid_t gid) {
    for (size_t i = 0; i < cache->group_count; i++) {
        if (cache->groups[i].gid == gid) {
            return cache->groups[i].name;
        }
    }

    struct group *gr = getgrgid(gid);
    const char *name = gr ? gr->gr_name : "?";

    if (cache->group_count < NAME_CACHE_LIMIT) {
        GroupCacheEntry *slot = &cache->groups[cache->group_count++];
        slot->gid = gid;
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        return slot->name;
    }

    return name;
}
#endif

static void owner_group_strings(const struct stat *st, NameCache *cache, char *user,
                                size_t user_size, char *group, size_t group_size) {
#ifdef _WIN32
    (void)st;
    (void)cache;

    snprintf(user, user_size, "-");
    snprintf(group, group_size, "-");
#else
    snprintf(user, user_size, "%s", cached_user_name(cache, st->st_uid));
    snprintf(group, group_size, "%s", cached_group_name(cache, st->st_gid));
#endif
}

static void format_time(time_t modified_time, char *timebuf, size_t timebuf_size) {
    struct tm tm_value;

    if (!localtime_safe(&modified_time, &tm_value)) {
        snprintf(timebuf, timebuf_size, "?");
        return;
    }

    if (strftime(timebuf, timebuf_size, "%b %d %H:%M", &tm_value) == 0) {
        snprintf(timebuf, timebuf_size, "?");
    }
}

static void print_long_from_stat(const char *type_path, const char *display_name,
                                 const struct stat *st, NameCache *cache) {
    char modes[11];
    mode_string(type_path, st->st_mode, modes);

    char user[NAME_FIELD_SIZE];
    char group[NAME_FIELD_SIZE];
    owner_group_strings(st, cache, user, sizeof(user), group, sizeof(group));

    char timebuf[64];
    format_time(st->st_mtime, timebuf, sizeof(timebuf));

    printf("%s %" PRIuMAX " %-8s %-8s %8" PRIdMAX " %s %s\n", modes, (uintmax_t)st->st_nlink, user,
           group, (intmax_t)st->st_size, timebuf, display_name);
}

static int print_single_path(const char *path, const Options *options, NameCache *cache) {
    struct stat st;

    if (stat_path_no_follow(path, &st) < 0) {
        perror(path);
        return -1;
    }

    if (options->long_format) {
        print_long_from_stat(path, path, &st, cache);
    } else {
        puts(path);
    }

    return 0;
}

static int compare_entry_ptrs_by_name(const void *a, const void *b) {
    const DirectoryEntry *const *entry_a = a;
    const DirectoryEntry *const *entry_b = b;

    return strcmp((*entry_a)->name, (*entry_b)->name);
}

static int make_entry_type_path(char *out, size_t out_size, const char *dir_path,
                                const DirectoryEntry *entry, const char **type_path) {
#ifdef _WIN32
    if (join_path_checked(out, out_size, dir_path, entry->name) < 0) {
        return -1;
    }

    *type_path = out;
#else
    (void)out;
    (void)out_size;
    (void)dir_path;
    (void)entry;

    // On POSIX, lstat/fstatat already cached the file type in st_mode.
    *type_path = NULL;
#endif

    return 0;
}

static int append_entry(DIR *dir, const char *dir_path, const char *name, const Options *options,
                        DirectoryEntry **entries, size_t *count, size_t *capacity,
                        unsigned long long *total_blocks) {
    if (*count >= *capacity) {
        if (grow_entries(entries, capacity) < 0) {
            perror("realloc");
            return -1;
        }
    }

    char *name_copy = copy_string(name);
    if (name_copy == NULL) {
        perror("malloc");
        return -1;
    }

    DirectoryEntry entry = {0};
    entry.name = name_copy;

    if (options->long_format) {
        if (stat_entry_in_open_dir(dir, dir_path, name_copy, &entry.st) < 0) {
            perror(name_copy);
            free(name_copy);
            return 0; // Skip this entry, but keep listing the rest.
        }

        entry.has_stat = 1;

#ifdef _WIN32
        char fullpath[PATH_BUFFER_SIZE];

        if (join_path_checked(fullpath, sizeof(fullpath), dir_path, name_copy) < 0) {
            perror(name_copy);
            free(name_copy);
            return 0;
        }

        entry.blocks = allocated_blocks_1024(fullpath, &entry.st);
#else
        entry.blocks = allocated_blocks_1024(NULL, &entry.st);
#endif

        *total_blocks += entry.blocks;
    }

    (*entries)[*count] = entry;
    (*count)++;
    return 0;
}

static int list_directory(const char *path, const Options *options, NameCache *cache) {
    DIR *dir = opendir(path);

    if (dir == NULL) {
        if (errno == ENOTDIR) {
            return print_single_path(path, options, cache);
        }

        perror(path);
        return -1;
    }

    size_t count = 0;
    size_t capacity = INITIAL_ENTRY_CAPACITY;

    DirectoryEntry *entries = malloc(capacity * sizeof(*entries));
    if (entries == NULL) {
        perror("malloc");
        closedir(dir);
        return -1;
    }

    unsigned long long total_blocks = 0;

    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0) {
                perror("readdir");
                closedir(dir);
                free_entries(entries, count);
                return -1;
            }

            break;
        }

        if (should_skip_name(options, entry->d_name)) {
            continue;
        }

        if (append_entry(dir, path, entry->d_name, options, &entries, &count, &capacity,
                         &total_blocks) < 0) {
            closedir(dir);
            free_entries(entries, count);
            return -1;
        }
    }

    if (closedir(dir) != 0) {
        perror("closedir");
        free_entries(entries, count);
        return -1;
    }

    DirectoryEntry **order = malloc(count * sizeof(*order));

    if (count > 0 && order == NULL) {
        perror("malloc");
        free_entries(entries, count);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        order[i] = &entries[i];
    }

    qsort(order, count, sizeof(order[0]), compare_entry_ptrs_by_name);

    if (options->long_format) {
        printf("total %llu\n", total_blocks);
    }

    for (size_t i = 0; i < count; i++) {
        const DirectoryEntry *current = order[i];

        if (options->long_format) {
            if (!current->has_stat) {
                continue;
            }

            char type_path_buffer[PATH_BUFFER_SIZE];
            const char *type_path = NULL;

            if (make_entry_type_path(type_path_buffer, sizeof(type_path_buffer), path, current,
                                     &type_path) < 0) {
                perror(current->name);
                continue;
            }

            print_long_from_stat(type_path, current->name, &current->st, cache);
        } else {
            puts(current->name);
        }
    }

    free(order);
    free_entries(entries, count);
    return 0;
}

int main(int argc, char *argv[]) {
    static char stdout_buffer[OUTPUT_BUFFER_SIZE];

    (void)setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));

    Options options;
    const char *path = NULL;

    int parse_result = parse_args(argc, argv, &options, &path);
    if (parse_result != 0) {
        return parse_result < 0 ? 1 : 0;
    }

    NameCache cache = {0};

    if (options.directory_itself) {
        return print_single_path(path, &options, &cache) == 0 ? 0 : 1;
    }

    return list_directory(path, &options, &cache) == 0 ? 0 : 1;
}
