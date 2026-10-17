/**
 * cp command for AiShell.
 * Copies files and directories, supporting recursive directory copies.
 *
 * Design notes:
 * - argtable3 is the single source of truth for both parsing and usage output.
 * - copy_node() dispatches based on lstat() result (symlink, directory, or regular file).
 * - Regular files are copied with open/read/write loops and fchmod to preserve mode bits.
 * - Directories are traversed recursively via opendir/readdir.
 * - The -r flag is required to copy directories; without it, directory operands
 *   are reported as errors (POSIX behavior).
 */

#define _POSIX_C_SOURCE 200809L

#include "argtable3.h"
#include "cmd_spec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** argtable3 argument table for cp.
 *  Layout: [help, recursive, paths, end, NULL] */
typedef struct cp_argtable {
    arg_lit_t *help;   /* -h/--help flag: prints usage and exits 0 */
    arg_lit_t *recursive;  /* -r/--recursive: copy directories recursively */
    arg_file_t *paths;     /* 2–100 file path operands */
    arg_end_t *end;        /* argtable3 terminator; collects parse errors */
    void *table[5];
} cp_argtable_t;

/** Build the argtable3 table for cp.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_cp_argtable(cp_argtable_t *args)
{
    args->help = arg_litn("h", "help", 0, 1, "show usage and description");
    args->recursive = arg_litn("r", "recursive", 0, 1,
                               "copy directories recursively");
    args->paths = arg_filen(NULL, NULL, "<source>... <destination>", 2, 100,
                            "source path(s) and destination");
    args->end = arg_end(20);
    args->table[0] = args->help;
    args->table[1] = args->recursive;
    args->table[2] = args->paths;
    args->table[3] = args->end;
    args->table[4] = NULL;

    if (args->help == NULL || args->recursive == NULL || args->paths == NULL ||
        args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }
    return 0;
}

/** Print cp usage text derived from the argtable3 table.
 *  Uses arg_print_syntax for the synopsis and arg_print_glossary for the
 *  option descriptions so they never diverge from parsed options. */
static void print_cp_usage(FILE *out, void **table)
{
    fputs("Usage: cp ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/** Public usage printer for the cp command. */
void cp_print_usage(FILE *out)
{
    cp_argtable_t args;

    if (out == NULL) {
        return;
    }
    if (build_cp_argtable(&args) != 0) {
        fputs("cp: failed to allocate argument table\n", stderr);
        return;
    }
    print_cp_usage(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Strip trailing slashes from a path (except for root "/").
 *  Required because lstat("path/", ...) follows the link, which we want to avoid
 *  when checking whether the destination is a directory. */
static char *trim_trailing_slashes(const char *path)
{
    size_t length = strlen(path);
    char *copy = malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, path, length + 1);
    /* Remove all trailing slashes except for the root directory. */
    while (length > 1 && copy[length - 1] == '/') {
        copy[--length] = '\0';
    }
    return copy;
}

/** Extract the basename (final path component) of a path.
 *  Returns a malloc'd string the caller must free. */
static char *path_basename(const char *path)
{
    size_t length = strlen(path);
    size_t start;
    char *base;

    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    if (length == 1 && path[0] == '/') {
        return strdup("/");
    }
    start = length;
    while (start > 0 && path[start - 1] != '/') {
        --start;
    }
    base = malloc(length - start + 1);
    if (base != NULL) {
        memcpy(base, path + start, length - start);
        base[length - start] = '\0';
    }
    return base;
}

/** Join a directory path and a name into "directory/name".
 *  Handles the case where directory does or doesn't end with '/'. */
static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    /* Determine whether a '/' separator is needed between directory and name. */
    int needs_slash = directory_length > 0 &&
                      directory[directory_length - 1] != '/';
    char *joined = malloc(directory_length + (size_t)needs_slash +
                          name_length + 1);

    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined, directory, directory_length);
    if (needs_slash) {
        joined[directory_length++] = '/';
    }
    memcpy(joined + directory_length, name, name_length + 1);
    return joined;
}

/** Check whether the destination inode matches the source inode.
 *  Uses lstat() (does not follow symlinks) on the destination to detect
 *  same-file cases. Returns 1 if same, 0 otherwise. */
static int same_file(const char *destination, const struct stat *source_stat)
{
    struct stat destination_stat;

    /* lstat retrieves metadata without following symlinks. */
    if (lstat(destination, &destination_stat) != 0) {
        return 0;
    }
    return source_stat->st_dev == destination_stat.st_dev &&
           source_stat->st_ino == destination_stat.st_ino;
}

/** Determine whether the target path is inside the source directory tree.
 *  Walks upward through the path components, calling stat() at each level.
 *  Returns 1 if target is inside source, 0 if not, -1 on error. */
static int target_is_within_source(const char *target,
                                  const struct stat *source_stat)
{
    char *candidate = strdup(target);

    if (candidate == NULL) {
        errno = ENOMEM;
        return -1;
    }
    for (;;) {
        struct stat candidate_stat;
        char *slash;

        /* stat() follows symlinks, so an intermediate link won't be confused. */
        if (stat(candidate, &candidate_stat) == 0 &&
            candidate_stat.st_dev == source_stat->st_dev &&
            candidate_stat.st_ino == source_stat->st_ino) {
            free(candidate);
            return 1;
        }
        slash = strrchr(candidate, '/');
        if (slash == NULL) {
            free(candidate);
            return 0;
        }
        if (slash == candidate) {
            if (candidate[1] == '\0') {
                free(candidate);
                return 0;
            }
            candidate[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

/** Copy a symbolic link by reading its target with readlink() and
 *  recreating the link with symlink(). Preserves link semantics. */
static int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat)
{
    /* Allocate a buffer sized to the link content (plus 1 for null terminator). */
    size_t capacity = source_stat->st_size > 0
                          ? (size_t)source_stat->st_size + 1
                          : 256;
    char *link_target = NULL;
    ssize_t length;
    struct stat destination_stat;

    /* Loop to handle the rare case where st_size is inaccurate (e.g., procfs). */
    for (;;) {
        char *resized = realloc(link_target, capacity);
        if (resized == NULL) {
            free(link_target);
            errno = ENOMEM;
            perror("cp");
            return -1;
        }
        link_target = resized;
        /* readlink reads the link target path into our buffer. */
        length = readlink(source, link_target, capacity);
        if (length < 0) {
            perror(source);
            free(link_target);
            return -1;
        }
        if ((size_t)length < capacity) {
            break;
        }
        if (capacity > (size_t)-1 / 2) {
            free(link_target);
            errno = ENAMETOOLONG;
            perror(source);
            return -1;
        }
        capacity *= 2;
    }
    link_target[length] = '\0';

    /* Check if destination already exists to handle same-file detection. */
    if (lstat(destination, &destination_stat) == 0) {
        if (source_stat->st_dev == destination_stat.st_dev &&
            source_stat->st_ino == destination_stat.st_ino) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n",
                    source, destination);
            free(link_target);
            return -1;
        }
        /* If destination is a directory, we cannot overwrite it with a link. */
        if (S_ISDIR(destination_stat.st_mode) || unlink(destination) != 0) {
            if (S_ISDIR(destination_stat.st_mode)) {
                errno = EISDIR;
            }
            perror(destination);
            free(link_target);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(destination);
        free(link_target);
        return -1;
    }

    /* symlink() creates a new symbolic link pointing to link_target. */
    if (symlink(link_target, destination) != 0) {
        perror(destination);
        free(link_target);
        return -1;
    }
    free(link_target);
    return 0;
}

/** Copy a regular file using open/read/write loops.
 *  Preserves the source file mode bits via fchmod after creation. */
static int copy_regular_file(const char *source, const char *destination,
                             const struct stat *source_stat)
{
    int input = -1;
    int output = -1;
    char buffer[16384]; /* I/O buffer size — large for throughput */
    int status = -1;
    struct stat destination_stat;

    if (same_file(destination, source_stat)) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n",
                source, destination);
        return -1;
    }
    /* Check destination existence to handle overwrite vs. directory cases. */
    if (lstat(destination, &destination_stat) == 0) {
        if (S_ISDIR(destination_stat.st_mode)) {
            errno = EISDIR;
            perror(destination);
            return -1;
        }
        if (S_ISLNK(destination_stat.st_mode) && unlink(destination) != 0) {
            perror(destination);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(destination);
        return -1;
    }

    /* open() the source file for reading only; O_RDONLY is implicit. */
    input = open(source, O_RDONLY);
    if (input < 0) {
        perror(source);
        return -1;
    }
    /* open() the destination for writing; O_CREAT creates if missing,
     * O_TRUNC truncates if existing. Mode bits copied from source. */
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC,
                  source_stat->st_mode & 07777);
    if (output < 0) {
        perror(destination);
        goto cleanup;
    }

    /* Transfer data in chunks: read from input, write to output. */
    for (;;) {
        /* read() may transfer fewer bytes than requested; loop until EOF. */
        ssize_t bytes_read = read(input, buffer, sizeof(buffer));
        size_t written = 0;

        if (bytes_read < 0) {
            /* EINTR is a retryable interruption (signal); continue the loop. */
            if (errno == EINTR) {
                continue;
            }
            perror(source);
            goto cleanup;
        }
        if (bytes_read == 0) {
            /* EOF reached on input. */
            break;
        }
        /* write() may transfer a partial chunk; loop until all bytes are written. */
        while (written < (size_t)bytes_read) {
            ssize_t bytes_written = write(output, buffer + written,
                                          (size_t)bytes_read - written);
            if (bytes_written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror(destination);
                goto cleanup;
            }
            if (bytes_written == 0) {
                /* Should not happen, but guard against infinite loops. */
                errno = EIO;
                perror(destination);
                goto cleanup;
            }
            written += (size_t)bytes_written;
        }
    }
    /* fchmod() sets the permission bits on the open file descriptor,
     * preserving the source file's mode (already set via open's mode arg,
     * but this handles the case where open applied umask). */
    if (fchmod(output, source_stat->st_mode & 07777) != 0) {
        perror(destination);
        goto cleanup;
    }
    status = 0;

cleanup:
    /* Always close both descriptors, preserving any error from the primary op. */
    if (input >= 0 && close(input) != 0) {
        perror(source);
        status = -1;
    }
    if (output >= 0 && close(output) != 0) {
        perror(destination);
        status = -1;
    }
    return status;
}

/* Forward declaration: copy_node is recursive (calls copy_directory which
 * calls copy_node). */
static int copy_node(const char *source, const char *destination, int recursive);

/** Recursively copy a directory tree.
 *  Creates the destination directory with mkdir() if it doesn't exist,
 *  then iterates over entries via opendir/readdir. */
static int copy_directory(const char *source, const char *destination,
                          const struct stat *source_stat)
{
    DIR *directory;
    struct dirent *entry;
    struct stat destination_stat;
    int created = 0;
    int status = 0;
    int within_source;

    if (same_file(destination, source_stat)) {
        fprintf(stderr, "cp: '%s' and '%s' are the same directory\n",
                source, destination);
        return -1;
    }
    within_source = target_is_within_source(destination, source_stat);
    if (within_source < 0) {
        perror("cp");
        return -1;
    }
    if (within_source) {
        fprintf(stderr, "cp: cannot copy '%s' into itself, '%s'\n",
                source, destination);
        return -1;
    }

    /* Check if destination exists; create it with mkdir() if missing.
     * lstat() avoids following symlinks to detect directory vs. symlink targets. */
    if (lstat(destination, &destination_stat) == 0) {
        if (!S_ISDIR(destination_stat.st_mode)) {
            errno = ENOTDIR;
            perror(destination);
            return -1;
        }
    } else if (errno == ENOENT) {
        /* mkdir() creates a new directory with the given mode bits. */
        if (mkdir(destination, source_stat->st_mode & 07777) != 0) {
            perror(destination);
            return -1;
        }
        created = 1;
    } else {
        perror(destination);
        return -1;
    }

    /* opendir() opens a directory stream for reading entries. */
    directory = opendir(source);
    if (directory == NULL) {
        perror(source);
        if (created) {
            (void)rmdir(destination); /* Best-effort cleanup. */
        }
        return -1;
    }

    /* readdir() returns one directory entry at a time; NULL with errno==0
     * signals end of stream; errno!=0 indicates an error. */
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *source_child;
        char *destination_child;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        source_child = join_path(source, entry->d_name);
        destination_child = join_path(destination, entry->d_name);
        if (source_child == NULL || destination_child == NULL) {
            fprintf(stderr, "cp: out of memory\n");
            free(source_child);
            free(destination_child);
            status = -1;
            break;
        }
        if (copy_node(source_child, destination_child, 1) != 0) {
            status = -1;
        }
        free(source_child);
        free(destination_child);
        if (status != 0) {
            break;
        }
        errno = 0;
    }
    /* After the loop, errno!=0 means readdir() encountered an error. */
    if (entry == NULL && errno != 0) {
        perror(source);
        status = -1;
    }
    /* closedir() frees the directory stream associated with the DIR pointer. */
    if (closedir(directory) != 0) {
        perror(source);
        status = -1;
    }
    /* Preserve source permissions on the new directory. */
    if (status == 0 && created &&
        chmod(destination, source_stat->st_mode & 07777) != 0) {
        perror(destination);
        status = -1;
    }
    return status;
}

/** Dispatch a single source path to the appropriate copy function
 *  based on its file type (determined via lstat). */
static int copy_node(const char *source, const char *destination, int recursive)
{
    struct stat source_stat;

    /* lstat() retrieves metadata without following symlinks — critical for
     * detecting and copying symlinks rather than their targets. */
    if (lstat(source, &source_stat) != 0) {
        perror(source);
        return -1;
    }
    if (S_ISLNK(source_stat.st_mode)) {
        return copy_symlink(source, destination, &source_stat);
    }
    if (S_ISDIR(source_stat.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n",
                    source);
            return -1;
        }
        return copy_directory(source, destination, &source_stat);
    }
    if (S_ISREG(source_stat.st_mode)) {
        return copy_regular_file(source, destination, &source_stat);
    }
    fprintf(stderr, "cp: unsupported file type: '%s'\n", source);
    return -1;
}

int cp_run(int argc, char **argv)
{
    cp_argtable_t args;
    int parse_errors;
    int status = 0;
    int source_count;
    int destination_is_directory;
    struct stat destination_stat;
    char *destination_path;
    int i;

    if (build_cp_argtable(&args) != 0) {
        fputs("cp: failed to allocate argument table\n", stderr);
        return 1;
    }
    parse_errors = arg_parse(argc, argv, args.table);
    if (args.help->count > 0) {
        print_cp_usage(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }
    if (parse_errors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_cp_usage(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    source_count = args.paths->count - 1;
    destination_path = trim_trailing_slashes(
        args.paths->filename[source_count]);
    if (destination_path == NULL) {
        fputs("cp: out of memory\n", stderr);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }
    /* stat() (follows symlinks) on destination to check if it's a directory. */
    destination_is_directory =
        stat(destination_path, &destination_stat) == 0 &&
        S_ISDIR(destination_stat.st_mode);
    if (source_count > 1 && !destination_is_directory) {
        fprintf(stderr, "cp: target '%s' is not a directory\n",
                destination_path);
        free(destination_path);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    for (i = 0; i < source_count; ++i) {
        char *source = trim_trailing_slashes(args.paths->filename[i]);
        char *target = NULL;

        if (source == NULL) {
            fputs("cp: out of memory\n", stderr);
            status = 1;
            break;
        }
        if (destination_is_directory) {
            char *base = path_basename(source);
            if (base == NULL) {
                fputs("cp: out of memory\n", stderr);
                free(source);
                status = 1;
                break;
            }
            target = join_path(destination_path, base);
            free(base);
        } else {
            target = strdup(destination_path);
        }
        if (target == NULL) {
            fputs("cp: out of memory\n", stderr);
            free(source);
            status = 1;
            break;
        }
        if (copy_node(source, target, args.recursive->count > 0) != 0) {
            status = 1;
        }
        free(source);
        free(target);
        if (status != 0) {
            break;
        }
    }

    free(destination_path);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_cp_spec = {
    "cp",
    "copy files and directories",
    "Copy SOURCE(s) to DESTINATION. Use -r to copy directories recursively.",
    cp_run,
    cp_print_usage
};

void register_cp_command(void)
{
    register_command(&cmd_cp_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_cp_spec.run(argc, argv);
}
#endif
