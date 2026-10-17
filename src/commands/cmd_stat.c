/**
 * stat command for AiShell.
 * Displays file metadata: size, permissions, owner, and timestamps.
 *
 * Design notes:
 * - argtable3 drives parsing and usage; no manual argument handling.
 * - Uses stat() (follows symlinks) to report metadata on the target file.
 * - Timestamps are formatted with localtime_r() + strftime() for thread safety.
 * - Owner and group names are resolved via getpwuid() and getgrgid(),
 *   with numeric fallback when the names cannot be looked up.
 * - --json mode emits structured output for autonomous agents.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/** argtable3 argument table for stat.
 *  Layout: [help, json, files, end, NULL] */
typedef struct stat_argtable {
    arg_lit_t *help;   /* -h/--help flag */
    arg_lit_t *json;   /* --json: machine-readable output for agents */
    arg_file_t *files; /* One or more file operands to inspect */
    arg_end_t *end;    /* argtable3 error terminator */
    void *table[5];
} stat_argtable_t;

/** Build the argtable3 table for stat.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_stat_argtable(stat_argtable_t *args)
{
    *args = (stat_argtable_t){0};
    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;
    args->json = arg_lit0(NULL, "json", "output in JSON format for agents");
    args->table[1] = args->json;
    args->files = arg_filen(NULL, NULL, "<file>", 1, 100, "file(s) to inspect");
    args->table[2] = args->files;
    args->end = arg_end(20);
    args->table[3] = args->end;
    args->table[4] = NULL;

    if (args->help == NULL || args->json == NULL || args->files == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }
    return 0;
}

/** Print stat usage derived from argtable3 definitions. */
static void print_stat_usage(FILE *out, void **table)
{
    fputs("Usage: stat ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void stat_print_usage(FILE *out)
{
    stat_argtable_t args;

    if (out == NULL) {
        return;
    }
    if (build_stat_argtable(&args) != 0) {
        fputs("stat: failed to allocate argument table\n", stderr);
        return;
    }
    print_stat_usage(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Format a mode_t into the 11-character POSIX permission string.
 *  Handles SUID, SGID, and sticky bit special cases.
 *  Example: "-rw-r--r--" for a regular file with 0644 permissions. */
static void format_stat_permissions(mode_t mode, char permissions[11])
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
    permissions[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T') :
                     ((mode & S_IXOTH) ? 'x' : '-');
#else
    permissions[9] = (mode & S_IXOTH) ? 'x' : '-';
#endif
    permissions[10] = '\0';
}

/** Get a human-readable file type string. */
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

/** Escape a string for safe JSON output. */
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

/** Print metadata for a single file in human-readable format.
 *  Uses stat() to retrieve the full struct stat, then resolves owner/group
 *  names and formats timestamps for human-readable output. */
static int print_stat_entry(const char *path)
{
    struct stat info;
    struct passwd *owner;
    struct tm accessed;
    struct tm modified;
    struct tm changed;
    char permissions[11];
    char access_time[64];
    char modify_time[64];
    char change_time[64];
    char owner_id[32];
    const char *owner_name;

    /* stat() retrieves file metadata; follows symlinks.
     * Returns -1 with errno set if the path cannot be resolved. */
    if (stat(path, &info) != 0) {
        perror(path);
        return -1;
    }

    /* getpwuid() looks up the user name for the file's owner UID.
     * Falls back to numeric UID string if the user is not in /etc/passwd. */
    owner = getpwuid(info.st_uid);
    if (owner == NULL) {
        (void)snprintf(owner_id, sizeof(owner_id), "%ju", (uintmax_t)info.st_uid);
        owner_name = owner_id;
    } else {
        owner_name = owner->pw_name;
    }

    /* localtime_r() converts epoch time to broken-down local time.
     * The _r suffix denotes the reentrant (thread-safe) version.
     * strftime() formats the time into a fixed-size buffer. */
    if (localtime_r(&info.st_atime, &accessed) == NULL ||
        strftime(access_time, sizeof(access_time), "%Y-%m-%d %H:%M:%S %z",
                 &accessed) == 0 ||
        localtime_r(&info.st_mtime, &modified) == NULL ||
        strftime(modify_time, sizeof(modify_time), "%Y-%m-%d %H:%M:%S %z",
                 &modified) == 0 ||
        localtime_r(&info.st_ctime, &changed) == NULL ||
        strftime(change_time, sizeof(change_time), "%Y-%m-%d %H:%M:%S %z",
                 &changed) == 0) {
        fprintf(stderr, "stat: could not format timestamps for %s\n", path);
        return -1;
    }

    format_stat_permissions(info.st_mode, permissions);

    /* Print all metadata in a structured, multi-line format. */
    if (printf("  File: %s\n"
               "  Size: %jd\n"
               "  Permissions: %04o (%s)\n"
               "  Owner: %s (uid %ju)\n"
               "  Access: %s\n"
               "  Modify: %s\n"
               "  Change: %s\n",
               path, (intmax_t)info.st_size, (unsigned int)(info.st_mode & 07777),
               permissions, owner_name, (uintmax_t)info.st_uid, access_time,
               modify_time, change_time) < 0) {
        perror("stat: stdout");
        return -1;
    }
    return 0;
}

/** Print metadata for a single file in JSON format for agent consumption. */
static int print_json_stat_entry(const char *path)
{
    struct stat info;
    struct passwd *owner;
    struct group *group;
    char permissions[11];
    char access_time[64];
    char modify_time[64];
    char change_time[64];
    struct tm accessed;
    struct tm modified;
    struct tm changed;
    char owner_id[32];
    char group_id[32];
    const char *owner_name;
    const char *group_name;

    /* stat() retrieves metadata; follows symlinks. */
    if (stat(path, &info) != 0) {
        perror(path);
        return -1;
    }

    /* Resolve owner username, fallback to numeric UID. */
    owner = getpwuid(info.st_uid);
    if (owner == NULL) {
        (void)snprintf(owner_id, sizeof(owner_id), "%ju", (uintmax_t)info.st_uid);
        owner_name = owner_id;
    } else {
        owner_name = owner->pw_name;
    }

    /* Resolve group name, fallback to numeric GID. */
    group = getgrgid(info.st_gid);
    if (group == NULL) {
        (void)snprintf(group_id, sizeof(group_id), "%ju", (uintmax_t)info.st_gid);
        group_name = group_id;
    } else {
        group_name = group->gr_name;
    }

    /* localtime_r() + strftime() for thread-safe timestamp formatting.
     * ISO 8601 format (YYYY-MM-DDTHH:MM:SS) is used for JSON output so
     * that agents can parse timestamps unambiguously. */
    if (localtime_r(&info.st_atime, &accessed) == NULL ||
        strftime(access_time, sizeof(access_time), "%Y-%m-%dT%H:%M:%S", &accessed) == 0 ||
        localtime_r(&info.st_mtime, &modified) == NULL ||
        strftime(modify_time, sizeof(modify_time), "%Y-%m-%dT%H:%M:%S", &modified) == 0 ||
        localtime_r(&info.st_ctime, &changed) == NULL ||
        strftime(change_time, sizeof(change_time), "%Y-%m-%dT%H:%M:%S", &changed) == 0) {
        fprintf(stderr, "stat: could not format timestamps for %s\n", path);
        return -1;
    }

    format_stat_permissions(info.st_mode, permissions);

    /* Emit a JSON object with all metadata fields for agent consumption.
     * No trailing newline — caller handles comma separation. */
    printf("  {\"file\": \"");
    json_escape_string(path);
    printf("\", \"type\": \"%s\", \"size\": %jd, \"permissions\": \"%s\", ",
           file_type_string(info.st_mode), (intmax_t)info.st_size, permissions);
    printf("\"octal_perms\": \"%04o\", \"owner\": \"%s\", \"uid\": %ju, ",
           (unsigned int)(info.st_mode & 07777), owner_name, (uintmax_t)info.st_uid);
    printf("\"group\": \"%s\", \"gid\": %ju, \"links\": %ju, ",
           group_name, (uintmax_t)info.st_gid, (uintmax_t)info.st_nlink);
    printf("\"access_time\": \"%s\", \"modify_time\": \"%s\", \"change_time\": \"%s\"}",
           access_time, modify_time, change_time);

    return 0;
}

int stat_run(int argc, char **argv)
{
    stat_argtable_t args;
    int nerrors;
    int status = 0;
    int i;

    if (build_stat_argtable(&args) != 0) {
        fputs("stat: failed to allocate argument table\n", stderr);
        return 1;
    }

    nerrors = arg_parse(argc, argv, args.table);

    if (args.help->count > 0) {
        print_stat_usage(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_stat_usage(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    if (args.json->count) {
        /* JSON mode: emit an array of objects.
         * Each entry is printed on one line, separated by commas. */
        printf("[\n");
        for (i = 0; i < args.files->count; ++i) {
            if (print_json_stat_entry(args.files->filename[i]) != 0) {
                status = 1;
                break;
            }
            /* Print newline after each entry; comma goes between entries. */
            printf("%s\n", (i < args.files->count - 1) ? "," : "");
        }
        printf("]\n");
    } else {
        /* Default mode: human-readable text output. */
        for (i = 0; i < args.files->count; ++i) {
            if (print_stat_entry(args.files->filename[i]) != 0) {
                status = 1;
            }
        }
    }

    /* Final flush to detect any pending write errors on stdout. */
    if (fflush(stdout) == EOF) {
        perror("stat: stdout");
        status = 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_stat_spec = {
    "stat",
    "display file metadata",
    "Display the size, permissions, owner, and access, modification, and change times of FILE(s).",
    stat_run,
    stat_print_usage
};

void register_stat_command(void)
{
    register_command(&cmd_stat_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_stat_spec.run(argc, argv);
}
#endif