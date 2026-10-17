/**
 * mv command for AiShell.
 * Moves/renames files and directories, with cross-filesystem support.
 *
 * Design notes:
 * - argtable3 drives parsing and usage.
 * - move_one() first attempts rename() for atomic same-filesystem moves.
 * - If rename() fails with EXDEV (cross-device link), falls back to
 *   copy-and-remove via copy_node() + remove_tree().
 * - copy_node() dispatches based on lstat() type: symlinks, directories,
 *   regular files, FIFOs, and rejects other special files.
 * - copy_regular_file() uses open/read/write with EINTR retry loops.
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

/** argtable3 argument table for mv.
 *  Layout: [help, paths, end, NULL] */
typedef struct mv_argtable {
    arg_lit_t *help;    /* -h/--help flag */
    arg_file_t *paths;  /* 2–100 path operands: source(s) and destination */
    arg_end_t *end;     /* argtable3 error terminator */
    void *table[4];
} mv_argtable_t;

/** Build the argtable3 table for mv.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_mv_argtable(mv_argtable_t *args)
{
    args->help = arg_litn("h", "help", 0, 1, "show usage and description");
    args->paths = arg_filen(NULL, NULL, "<source>... <destination>", 2, 100,
                            "source path(s) and destination");
    args->end = arg_end(20);
    args->table[0] = args->help;
    args->table[1] = args->paths;
    args->table[2] = args->end;
    args->table[3] = NULL;

    if (args->help == NULL || args->paths == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }
    return 0;
}

/** Print mv usage derived from argtable3 definitions. */
static void print_mv_usage(FILE *out, void **table)
{
    fputs("Usage: mv ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void mv_print_usage(FILE *out)
{
    mv_argtable_t args;

    if (out == NULL) {
        return;
    }
    if (build_mv_argtable(&args) != 0) {
        fputs("mv: failed to allocate argument table\n", stderr);
        return;
    }
    print_mv_usage(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Strip trailing slashes from a path (except for root "/").
 *  Prevents stat/lstat from following directory symlinks unexpectedly. */
static char *trim_trailing_slashes(const char *path)
{
    size_t length = strlen(path);
    char *copy = malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, path, length + 1);
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
 *  Ensures exactly one '/' between components. */
static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
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

/** Recursively remove a path and all its contents.
 *  Used by move_one() after cross-device copy to delete the source. */
static int remove_tree(const char *path)
{
    struct stat st;

    /* lstat() retrieves metadata without following symlinks. */
    if (lstat(path, &st) != 0) {
        perror(path);
        return -1;
    }

    /* Non-directory: just unlink() the entry. */
    if (!S_ISDIR(st.st_mode)) {
        /* unlink() removes a directory entry (deletes the file). */
        if (unlink(path) != 0) {
            perror(path);
            return -1;
        }
        return 0;
    }

    /* Directory: open, recursively remove children, then remove the dir. */
    {
        /* opendir() opens a directory stream for iteration. */
        DIR *directory = opendir(path);
        struct dirent *entry;
        int status = 0;

        if (directory == NULL) {
            perror(path);
            return -1;
        }

        errno = 0;
        /* readdir() returns one entry at a time; NULL means EOF or error. */
        while ((entry = readdir(directory)) != NULL) {
            char *child;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            child = join_path(path, entry->d_name);
            if (child == NULL) {
                fputs("mv: out of memory\n", stderr);
                status = -1;
                break;
            }
            /* Recursively remove each child. */
            if (remove_tree(child) != 0) {
                status = -1;
            }
            free(child);
            if (status != 0) {
                break;
            }
            errno = 0;
        }

        /* Check if the loop ended due to a readdir() error (not just EOF). */
        if (entry == NULL && errno != 0) {
            perror(path);
            status = -1;
        }

        /* closedir() releases the directory stream. */
        if (closedir(directory) != 0) {
            perror(path);
            status = -1;
        }

        if (status != 0) {
            return -1;
        }
    }

    /* rmdir() removes the now-empty directory. */
    if (rmdir(path) != 0) {
        perror(path);
        return -1;
    }
    return 0;
}

/* Forward declarations for mutually recursive functions. */
static int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat);
static int copy_directory(const char *source, const char *destination,
                          const struct stat *source_stat);
static int copy_node(const char *source, const char *destination);

/** Copy a symbolic link by reading its target with readlink() and
 *  recreating the link with symlink(). Preserves the link, not the target. */
static int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat)
{
    /* Allocate buffer sized to st_size + 1; fall back to 256 if unknown. */
    size_t capacity = source_stat->st_size > 0
                          ? (size_t)source_stat->st_size + 1
                          : 256;
    char *target = NULL;
    ssize_t length;
    struct stat destination_stat;

    /* Loop to handle the rare case where st_size is inaccurate. */
    for (;;) {
        char *resized = realloc(target, capacity);
        if (resized == NULL) {
            free(target);
            errno = ENOMEM;
            perror("mv");
            return -1;
        }
        target = resized;
        /* readlink() reads the symlink target path into our buffer.
         * Returns the number of bytes placed in the buffer (no null terminator). */
        length = readlink(source, target, capacity);
        if (length < 0) {
            perror(source);
            free(target);
            return -1;
        }
        if ((size_t)length < capacity) {
            break;
        }
        if (capacity > (size_t)-1 / 2) {
            free(target);
            errno = ENAMETOOLONG;
            perror(source);
            return -1;
        }
        capacity *= 2;
    }
    target[length] = '\0';

    /* Check for same-file before creating the symlink. */
    if (lstat(destination, &destination_stat) == 0) {
        if (source_stat->st_dev == destination_stat.st_dev &&
            source_stat->st_ino == destination_stat.st_ino) {
            fprintf(stderr, "mv: '%s' and '%s' are the same file\n",
                    source, destination);
            free(target);
            return -1;
        }
        /* If destination is a directory or existing link, remove it first. */
        if (S_ISDIR(destination_stat.st_mode) || unlink(destination) != 0) {
            if (S_ISDIR(destination_stat.st_mode)) {
                errno = EISDIR;
            }
            perror(destination);
            free(target);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(destination);
        free(target);
        return -1;
    }

    /* symlink() creates a new symbolic link pointing to `target`. */
    if (symlink(target, destination) != 0) {
        perror(destination);
        free(target);
        return -1;
    }
    free(target);
    return 0;
}

/** Copy a regular file using open/read/write loops.
 *  Preserves mode bits via fchmod() after creation. */
static int copy_regular_file(const char *source, const char *destination,
                             const struct stat *source_stat)
{
    int input = -1;
    int output = -1;
    int status = -1;
    char buffer[16384]; /* I/O buffer size — large for throughput */
    struct stat destination_stat;
    /* lstat() checks destination existence without following symlinks. */
    int destination_exists = lstat(destination, &destination_stat) == 0;

    if (destination_exists &&
        source_stat->st_dev == destination_stat.st_dev &&
        source_stat->st_ino == destination_stat.st_ino) {
        fprintf(stderr, "mv: '%s' and '%s' are the same file\n",
                source, destination);
        return -1;
    }
    /* Cannot write into a directory as a file. */
    if (destination_exists && S_ISDIR(destination_stat.st_mode)) {
        errno = EISDIR;
        perror(destination);
        return -1;
    }
    /* If destination is a symlink, remove it before creating the regular file. */
    if (destination_exists && S_ISLNK(destination_stat.st_mode) &&
        unlink(destination) != 0) {
        perror(destination);
        return -1;
    }
    if (!destination_exists && errno != ENOENT) {
        perror(destination);
        return -1;
    }

    /* open() source for reading; O_RDONLY is implicit. */
    input = open(source, O_RDONLY);
    if (input < 0) {
        perror(source);
        return -1;
    }
    /* open() destination for writing; O_CREAT | O_TRUNC creates or truncates.
     * Mode bits from source are applied via fchmod later. */
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC,
                  source_stat->st_mode & 07777);
    if (output < 0) {
        perror(destination);
        goto cleanup;
    }

    /* Transfer data in chunks, handling partial reads and writes. */
    for (;;) {
        /* read() returns -1 on error, 0 on EOF, >0 with bytes read.
         * EINTR (signal interruption) is retried. */
        ssize_t bytes_read = read(input, buffer, sizeof(buffer));
        size_t written = 0;

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror(source);
            goto cleanup;
        }
        if (bytes_read == 0) {
            break; /* EOF */
        }

        /* write() may transfer a partial chunk; loop until all bytes are written.
         * EINTR is retried; a zero return is treated as an I/O error. */
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
                errno = EIO;
                perror(destination);
                goto cleanup;
            }
            written += (size_t)bytes_written;
        }
    }

    /* fchmod() sets permission bits on the open descriptor, bypassing umask. */
    if (fchmod(output, source_stat->st_mode & 07777) != 0) {
        perror(destination);
        goto cleanup;
    }
    status = 0;

cleanup:
    /* close() releases the file descriptor; check for errors. */
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

/** Recursively copy a directory tree.
 *  Creates the destination with mkdir(), iterates with opendir/readdir,
 *  and copies each child via copy_node(). */
static int copy_directory(const char *source, const char *destination,
                          const struct stat *source_stat)
{
    DIR *directory;
    struct dirent *entry;
    struct stat destination_stat;
    int created = 0;
    int status = 0;

    /* lstat() checks destination existence without following symlinks. */
    if (lstat(destination, &destination_stat) == 0) {
        if (!S_ISDIR(destination_stat.st_mode)) {
            errno = ENOTDIR;
            perror(destination);
            return -1;
        }
    } else if (errno == ENOENT) {
        /* mkdir() creates a new directory with source's permission bits. */
        if (mkdir(destination, source_stat->st_mode & 07777) != 0) {
            perror(destination);
            return -1;
        }
        created = 1;
    } else {
        perror(destination);
        return -1;
    }

    /* opendir() opens a directory stream for iteration. */
    directory = opendir(source);
    if (directory == NULL) {
        perror(source);
        if (created) {
            (void)rmdir(destination); /* Best-effort cleanup. */
        }
        return -1;
    }

    errno = 0;
    /* readdir() returns one entry per call; NULL means EOF or error. */
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
            fputs("mv: out of memory\n", stderr);
            free(source_child);
            free(destination_child);
            status = -1;
            break;
        }
        if (copy_node(source_child, destination_child) != 0) {
            status = -1;
        }
        free(source_child);
        free(destination_child);
        if (status != 0) {
            break;
        }
        errno = 0;
    }

    /* Check if readdir() failed (NULL return with errno set). */
    if (entry == NULL && errno != 0) {
        perror(source);
        status = -1;
    }

    /* closedir() releases the directory stream. */
    if (closedir(directory) != 0) {
        perror(source);
        status = -1;
    }

    /* Preserve source directory permissions on the copy. */
    if (status == 0 && created &&
        chmod(destination, source_stat->st_mode & 07777) != 0) {
        perror(destination);
        status = -1;
    }

    /* If copy failed and we created the directory, clean it up. */
    if (status != 0 && created) {
        (void)remove_tree(destination);
    }
    return status;
}

/** Copy a single node (file, symlink, directory, FIFO) from source to destination.
 *  Dispatches based on the file type from lstat(). */
static int copy_node(const char *source, const char *destination)
{
    struct stat source_stat;

    /* lstat() retrieves metadata without following symlinks. */
    if (lstat(source, &source_stat) != 0) {
        perror(source);
        return -1;
    }
    if (S_ISLNK(source_stat.st_mode)) {
        return copy_symlink(source, destination, &source_stat);
    }
    if (S_ISDIR(source_stat.st_mode)) {
        return copy_directory(source, destination, &source_stat);
    }
    if (S_ISREG(source_stat.st_mode)) {
        return copy_regular_file(source, destination, &source_stat);
    }
    /* mkfifo() creates a named pipe (FIFO) preserving mode bits. */
    if (S_ISFIFO(source_stat.st_mode)) {
        if (mkfifo(destination, source_stat.st_mode & 07777) != 0) {
            perror(destination);
            return -1;
        }
        return 0;
    }
    /* Other special files (block/char devices, sockets) are not supported
     * across filesystems. */
    fprintf(stderr, "mv: cannot copy special file '%s' across filesystems\n",
            source);
    errno = ENOTSUP;
    return -1;
}

/** Move a single source path to destination.
 *  First tries rename() for atomic same-filesystem moves.
 *  If rename() returns EXDEV (cross-device link), falls back to
 *  copy-and-remove: copy_node() + remove_tree(). */
static int move_one(const char *source, const char *destination)
{
    struct stat source_stat;
    struct stat destination_stat;
    int destination_result;

    /* lstat() to check source metadata without following symlinks. */
    if (lstat(source, &source_stat) != 0) {
        perror(source);
        return -1;
    }

    /* Check if destination already exists (and is the same file). */
    destination_result = lstat(destination, &destination_stat);
    if (destination_result == 0 &&
        source_stat.st_dev == destination_stat.st_dev &&
        source_stat.st_ino == destination_stat.st_ino) {
        fprintf(stderr, "mv: '%s' and '%s' are the same file\n",
                source, destination);
        return -1;
    }
    if (destination_result != 0 && errno != ENOENT) {
        perror(destination);
        return -1;
    }

    /* rename() atomically renames or moves within the same filesystem. */
    if (rename(source, destination) == 0) {
        return 0;
    }

    /* EXDEV means cross-device link — fallback to copy + remove. */
    if (errno != EXDEV) {
        perror(destination);
        return -1;
    }

    /* Cross-device: copy the entire tree, then remove the original. */
    if (copy_node(source, destination) != 0) {
        return -1;
    }
    return remove_tree(source);
}

int mv_run(int argc, char **argv)
{
    mv_argtable_t args;
    int parse_errors;
    int source_count;
    int destination_is_directory;
    int status = 0;
    int i;
    struct stat destination_stat;
    char *destination;

    if (build_mv_argtable(&args) != 0) {
        fputs("mv: failed to allocate argument table\n", stderr);
        return 1;
    }

    parse_errors = arg_parse(argc, argv, args.table);

    if (args.help->count > 0) {
        print_mv_usage(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    if (parse_errors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_mv_usage(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    source_count = args.paths->count - 1;
    destination = trim_trailing_slashes(args.paths->filename[source_count]);
    if (destination == NULL) {
        fputs("mv: out of memory\n", stderr);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* stat() (follows symlinks) to check if destination is a directory. */
    destination_is_directory =
        stat(destination, &destination_stat) == 0 &&
        S_ISDIR(destination_stat.st_mode);

    if (source_count > 1 && !destination_is_directory) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", destination);
        free(destination);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    for (i = 0; i < source_count; ++i) {
        char *source = trim_trailing_slashes(args.paths->filename[i]);
        char *target;

        if (source == NULL) {
            fputs("mv: out of memory\n", stderr);
            status = 1;
            break;
        }
        if (destination_is_directory) {
            char *base = path_basename(source);
            if (base == NULL) {
                fputs("mv: out of memory\n", stderr);
                free(source);
                status = 1;
                break;
            }
            target = join_path(destination, base);
            free(base);
        } else {
            target = strdup(destination);
        }
        if (target == NULL) {
            fputs("mv: out of memory\n", stderr);
            free(source);
            status = 1;
            break;
        }

        /* Skip no-op moves where source and target are identical paths. */
        if (strcmp(source, target) != 0 && move_one(source, target) != 0) {
            status = 1;
        }

        free(source);
        free(target);
        if (status != 0) {
            break;
        }
    }

    free(destination);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_mv_spec = {
    "mv",
    "move or rename files and directories",
    "Move SOURCE(s) to DESTINATION. Cross-filesystem moves use copy-and-remove.",
    mv_run,
    mv_print_usage
};

void register_mv_command(void)
{
    register_command(&cmd_mv_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_mv_spec.run(argc, argv);
}
#endif
