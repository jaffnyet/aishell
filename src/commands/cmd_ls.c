/**
 * ls command for AiShell.
 * Lists directory contents with support for -a (all), -l (long format),
 * and --json (machine-readable JSON output) for agent consumption.
 *
 * Design notes:
 * - argtable3 drives parsing and usage; no manual argument handling.
 * - fstatat() is used with AT_SYMLINK_NOFOLLOW instead of stat() so that
 *   symbolic links are reported as links rather than their targets.
 * - Directory entries are collected into a growable array, sorted with
 *   qsort(), then printed. This enables deterministic output ordering.
 * - Long format uses dirfd() from the open directory stream to resolve
 *   relative entry names without chdir().
 * - --json mode emits structured output for use by autonomous agents,
 *   producing an array of objects with name, type, permissions, size,
 *   owner, group, and modification time fields.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/** argtable3 argument table for ls.
 *  Layout: [all, long_format, json, help, directory, end, NULL] */
typedef struct ls_argtable {
    arg_lit_t *all;         /* -a/--all: include hidden entries (. and ..) */
    arg_lit_t *long_format; /* -l: long listing format with metadata */
    arg_lit_t *json;        /* --json: machine-readable JSON output */
    arg_lit_t *help;        /* -h/--help: show usage */
    arg_file_t *directory;  /* Optional directory operand (default: ".") */
    arg_end_t *end;         /* argtable3 error terminator */
    void *table[7];
} ls_argtable_t;

/** Simple entry struct for collecting directory names before sorting. */
typedef struct ls_entry {
    char *name;
} ls_entry_t;

/** Build the argtable3 table for ls.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_ls_argtable(ls_argtable_t *args)
{
    *args = (ls_argtable_t){0};

    args->all = arg_lit0("a", "all", "list all entries, including . and ..");
    args->table[0] = args->all;
    args->long_format = arg_lit0("l", NULL, "use long listing format");
    args->table[1] = args->long_format;
    args->json = arg_lit0(NULL, "json", "output in JSON format for agents");
    args->table[2] = args->json;
    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[3] = args->help;
    args->directory = arg_file0(NULL, NULL, "<dir>", "directory to list");
    args->table[4] = args->directory;
    args->end = arg_end(20);
    args->table[5] = args->end;
    args->table[6] = NULL;

    if (args->all == NULL || args->long_format == NULL || args->json == NULL ||
        args->help == NULL || args->directory == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/** Print ls usage derived from argtable3 definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: ls ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void ls_print_usage(FILE *out)
{
    ls_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_ls_argtable(&args) != 0) {
        fputs("ls: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** qsort comparator: sorts entries alphabetically by name. */
static int compare_entries(const void *left, const void *right)
{
    const ls_entry_t *entry_left = left;
    const ls_entry_t *entry_right = right;

    return strcmp(entry_left->name, entry_right->name);
}

/** Format a mode_t into the 11-character POSIX permission string.
 *  Example: "drwxr-xr-x" for a directory with 0755 permissions.
 *  Handles SUID, SGID, and sticky bit special cases. */
static void format_permissions(mode_t mode, char permissions[11])
{
    permissions[0] = S_ISDIR(mode) ? 'd' :
                     S_ISLNK(mode) ? 'l' :
                     S_ISCHR(mode) ? 'c' :
                     S_ISBLK(mode) ? 'b' :
                     S_ISFIFO(mode) ? 'p' :
                     S_ISSOCK(mode) ? 's' : '-';
    permissions[1] = (mode & S_IRUSR) ? 'r' : '-';
    permissions[2] = (mode & S_IWUSR) ? 'w' : '-';
    permissions[3] = (mode & S_ISUID) ? ((mode & S_IXUSR) ? 's' : 'S') :
                     ((mode & S_IXUSR) ? 'x' : '-');
    permissions[4] = (mode & S_IRGRP) ? 'r' : '-';
    permissions[5] = (mode & S_IWGRP) ? 'w' : '-';
    permissions[6] = (mode & S_ISGID) ? ((mode & S_IXGRP) ? 's' : 'S') :
                     ((mode & S_IXGRP) ? 'x' : '-');
    permissions[7] = (mode & S_IROTH) ? 'r' : '-';
    permissions[8] = (mode & S_IWOTH) ? 'w' : '-';
#ifdef S_ISVTX
    /* Sticky bit on directories: 't' if world-executable, 'T' if not. */
    permissions[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T') :
                     ((mode & S_IXOTH) ? 'x' : '-');
#else
    permissions[9] = (mode & S_IXOTH) ? 'x' : '-';
#endif
    permissions[10] = '\0';
}

/** Determine the type string for a file mode (for JSON output). */
static const char *file_type_string(mode_t mode)
{
    if (S_ISDIR(mode))  return "directory";
    if (S_ISLNK(mode))  return "symlink";
    if (S_ISCHR(mode))  return "char_device";
    if (S_ISBLK(mode))  return "block_device";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    if (S_ISREG(mode))  return "file";
    return "unknown";
}

/** Escape a string for safe JSON output (handles quotes and backslashes). */
static void json_escape_string(const char *s)
{
    for (; *s != '\0'; ++s) {
        switch (*s) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout);  break;
            case '\t': fputs("\\t", stdout);  break;
            default:   putchar(*s);           break;
        }
    }
}

/** Print one entry in long (-l) format.
 *  Uses fstatat() with the directory's file descriptor (via dirfd())
 *  to resolve the entry name relative to the open directory, avoiding
 *  a chdir() call. */
static int print_long_entry(DIR *directory, const char *name)
{
    struct stat info;
    struct passwd *owner;
    struct group *group;
    char permissions[11];
    char owner_id[32];
    char group_id[32];
    char timestamp[32];
    struct tm *modified;

    /* fstatat() retrieves the entry's metadata relative to the directory fd.
     * AT_SYMLINK_NOFOLLOW ensures symlinks are reported, not their targets. */
    if (fstatat(dirfd(directory), name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        perror(name);
        return -1;
    }

    format_permissions(info.st_mode, permissions);

    /* Look up the owner name from the UID; fall back to numeric UID if not found. */
    owner = getpwuid(info.st_uid);
    if (owner == NULL) {
        (void)snprintf(owner_id, sizeof(owner_id), "%ju", (uintmax_t)info.st_uid);
    }

    /* Look up the group name from the GID; fall back to numeric GID if not found. */
    group = getgrgid(info.st_gid);
    if (group == NULL) {
        (void)snprintf(group_id, sizeof(group_id), "%ju", (uintmax_t)info.st_gid);
    }

    /* localtime_r() converts the epoch timestamp to a broken-down local time.
     * strftime() formats it into "Mon DD HH:MM" (e.g., "Oct 10 14:30"). */
    modified = localtime(&info.st_mtime);
    if (modified == NULL ||
        strftime(timestamp, sizeof(timestamp), "%b %e %H:%M", modified) == 0) {
        (void)snprintf(timestamp, sizeof(timestamp), "??? ?? ??:??");
    }

    /* Print the long listing: permissions, links, owner, group, size, date, name. */
    if (printf("%s %3ju %-8s %-8s %8ju %s %s\n",
               permissions,
               (uintmax_t)info.st_nlink,
               owner != NULL ? owner->pw_name : owner_id,
               group != NULL ? group->gr_name : group_id,
               (uintmax_t)info.st_size,
               timestamp,
               name) < 0) {
        perror("ls: stdout");
        return -1;
    }

    return 0;
}

/** Print one entry in JSON format for agent consumption. */
static int print_json_entry(DIR *directory, const char *name)
{
    struct stat info;
    struct passwd *owner;
    struct group *group;
    char permissions[11];
    char timestamp[64];
    struct tm *modified;

    /* fstatat() retrieves metadata relative to the directory fd. */
    if (fstatat(dirfd(directory), name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        perror(name);
        return -1;
    }

    format_permissions(info.st_mode, permissions);

    owner = getpwuid(info.st_uid);
    group = getgrgid(info.st_gid);

    modified = localtime(&info.st_mtime);
    if (modified == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", modified) == 0) {
        /* ISO 8601 timestamp for easy parsing by agents.
         * On failure, leave timestamp empty rather than printing garbage. */
        timestamp[0] = '\0';
    }

    /* Emit a JSON object for this entry (no trailing newline;
     * the caller handles separation between entries). */
    printf("  {\"name\": \"");
    json_escape_string(name);
    printf("\", \"type\": \"%s\", \"permissions\": \"%s\", \"size\": %ju, ",
           file_type_string(info.st_mode), permissions,
           (uintmax_t)info.st_size);
    printf("\"owner\": \"%s\", \"group\": \"%s\", \"mtime\": \"%s\"}",
           owner != NULL ? owner->pw_name : "unknown",
           group != NULL ? group->gr_name : "unknown",
           timestamp);

    return 0;
}

/** Append a name to the growable entries array.
 *  Doubles capacity when full to amortize realloc cost. */
static int append_entry(ls_entry_t **entries, size_t *count, size_t *capacity,
                        const char *name)
{
    ls_entry_t *grown;
    char *copy;
    size_t name_length = strlen(name) + 1;

    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;

        if (new_capacity < *capacity ||
            new_capacity > SIZE_MAX / sizeof(**entries)) {
            errno = ENOMEM;
            return -1;
        }

        /* realloc() grows the existing buffer; old buffer is freed automatically. */
        grown = realloc(*entries, new_capacity * sizeof(**entries));
        if (grown == NULL) {
            return -1;
        }
        *entries = grown;
        *capacity = new_capacity;
    }

    copy = malloc(name_length);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, name, name_length);

    (*entries)[*count].name = copy;
    ++*count;
    return 0;
}

/** Free all entry names and the entries array itself. */
static void free_entries(ls_entry_t *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        free(entries[i].name);
    }
    free(entries);
}

int ls_run(int argc, char **argv)
{
    ls_argtable_t args;
    DIR *directory;
    struct dirent *entry;
    ls_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    size_t i;
    const char *directory_path;
    int status = 0;
    int nerrors;

    if (build_ls_argtable(&args) != 0) {
        fputs("ls: failed to allocate argument table\n", stderr);
        return 1;
    }

    /* arg_parse() processes argv against the argtable3 definitions;
     * returns the count of parse errors. */
    nerrors = arg_parse(argc, argv, args.table);

    /* Help flag takes priority over syntax errors. */
    if (args.help->count > 0) {
        print_usage_with_argtable(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    /* On syntax errors, print to stderr and exit non-zero. */
    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Default to current directory if no directory operand was given. */
    directory_path = args.directory->count > 0 ?
                     args.directory->filename[0] : ".";

    /* opendir() opens a directory stream; returns NULL with errno set on failure. */
    directory = opendir(directory_path);
    if (directory == NULL) {
        perror(directory_path);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Collect all directory entries into a growable array. */
    for (;;) {
        errno = 0;
        /* readdir() returns one entry at a time; NULL means EOF or error.
         * We reset errno before each call to distinguish EOF from error. */
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                perror("ls: readdir");
                status = 1;
            }
            break;
        }

        /* Skip hidden entries unless -a was specified. */
        if (!args.all->count && entry->d_name[0] == '.') {
            continue;
        }

        if (append_entry(&entries, &entry_count, &entry_capacity,
                         entry->d_name) != 0) {
            perror("ls: allocate directory entry");
            status = 1;
            break;
        }
    }

    if (status == 0) {
        /* Sort entries alphabetically for deterministic output. */
        if (entry_count > 1) {
            qsort(entries, entry_count, sizeof(*entries), compare_entries);
        }

        if (args.json->count) {
            /* JSON mode: emit an array of objects.
             * Each entry is printed on one line, separated by commas. */
            printf("[\n");
            for (i = 0; i < entry_count; ++i) {
                if (print_json_entry(directory, entries[i].name) != 0) {
                    status = 1;
                    break;
                }
                /* Print newline after each entry; comma goes between entries. */
                printf("%s\n", (i < entry_count - 1) ? "," : "");
            }
            printf("]\n");
        } else {
            for (i = 0; i < entry_count; ++i) {
                if (args.long_format->count) {
                    if (print_long_entry(directory, entries[i].name) != 0) {
                        status = 1;
                        break;
                    }
                } else if (printf("%s\n", entries[i].name) < 0) {
                    /* printf failure (e.g., broken pipe on stdout). */
                    perror("ls: stdout");
                    status = 1;
                    break;
                }

                /* Check for write errors after each printf (e.g., broken pipe). */
                if (ferror(stdout)) {
                    perror("ls: stdout");
                    status = 1;
                    break;
                }
            }
        }
    }

    /* closedir() releases the directory stream allocated by opendir(). */
    if (closedir(directory) != 0) {
        perror("ls: closedir");
        status = 1;
    }

    free_entries(entries, entry_count);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));

    /* Final flush to detect any pending write errors on stdout. */
    if (status == 0 && fflush(stdout) == EOF) {
        perror("ls: stdout");
        status = 1;
    }
    return status;
}

cmd_spec_t cmd_ls_spec = {
    "ls",
    "list directory contents",
    "List information about files and directories in standard POSIX format.",
    ls_run,
    ls_print_usage
};

void register_ls_command(void)
{
    register_command(&cmd_ls_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_ls_spec.run(argc, argv);
}
#endif
