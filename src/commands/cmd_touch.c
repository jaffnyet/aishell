/**
 * touch command for AiShell.
 * Updates file access and modification times, creating the file if absent.
 *
 * Design notes:
 * - argtable3 drives parsing and usage.
 * - utimensat() with NULL times sets timestamps to the current time.
 * - If the file doesn't exist (ENOENT), it is created with open() + close()
 *   and then utimensat() is called a second time.
 * - AT_FDCWD means the path is interpreted relative to the current directory.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/** argtable3 argument table for touch.
 *  Layout: [help, files, end, NULL] */
typedef struct touch_argtable {
    arg_lit_t *help;   /* -h/--help flag */
    arg_file_t *files; /* One or more file operands */
    arg_end_t *end;    /* argtable3 error terminator */
    void *table[4];
} touch_argtable_t;

/** Build the argtable3 table for touch.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_touch_argtable(touch_argtable_t *args)
{
    *args = (touch_argtable_t){0};
    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;
    args->files = arg_filen(NULL, NULL, "<file>", 1, 100, "file(s) to update or create");
    args->table[1] = args->files;
    args->end = arg_end(20);
    args->table[2] = args->end;
    args->table[3] = NULL;

    if (args->help == NULL || args->files == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }
    return 0;
}

/** Print touch usage derived from argtable3 definitions. */
static void print_touch_usage(FILE *out, void **table)
{
    fputs("Usage: touch ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void touch_print_usage(FILE *out)
{
    touch_argtable_t args;

    if (out == NULL) {
        return;
    }
    if (build_touch_argtable(&args) != 0) {
        fputs("touch: failed to allocate argument table\n", stderr);
        return;
    }
    print_touch_usage(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Update timestamps on a single file, creating it if it doesn't exist.
 *  First tries utimensat() to set times to current time; if the file
 *  doesn't exist, creates it with open() + close(), then calls utimensat() again.
 *  Returns 0 on success, -1 on failure (errno set by the failing syscall). */
static int touch_file(const char *path)
{
    int descriptor;

    /* utimensat() sets access and modification times to the current time
     * when times is NULL. AT_FDCWD means use the current working directory
     * as the base for relative paths. The flags argument 0 means no special
     * behavior (don't follow symlinks, but don't create the file either). */
    if (utimensat(AT_FDCWD, path, NULL, 0) == 0) {
        return 0;
    }
    /* If the file doesn't exist, fall through to create it. */
    if (errno != ENOENT) {
        return -1;
    }

    /* open() creates the file if it doesn't exist (O_CREAT).
     * O_WRONLY opens for writing only; O_NONBLOCK prevents blocking on FIFOs.
     * Mode 0666 is the default; umask will restrict the final permissions. */
    descriptor = open(path, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
    if (descriptor < 0) {
        return -1;
    }
    /* close() flushes and releases the file descriptor. */
    if (close(descriptor) != 0) {
        return -1;
    }
    /* Now that the file exists, set its timestamps to the current time. */
    return utimensat(AT_FDCWD, path, NULL, 0);
}

int touch_run(int argc, char **argv)
{
    touch_argtable_t args;
    int nerrors;
    int status = 0;
    int i;

    if (build_touch_argtable(&args) != 0) {
        fputs("touch: failed to allocate argument table\n", stderr);
        return 1;
    }

    nerrors = arg_parse(argc, argv, args.table);

    if (args.help->count > 0) {
        print_touch_usage(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_touch_usage(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Process each file operand: create or update timestamp. */
    for (i = 0; i < args.files->count; ++i) {
        const char *path = args.files->filename[i];

        if (touch_file(path) != 0) {
            perror(path);
            status = 1;
        }
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_touch_spec = {
    "touch",
    "create files or update timestamps",
    "Set the access and modification times of FILE(s) to the current time, creating missing files.",
    touch_run,
    touch_print_usage
};

void register_touch_command(void)
{
    register_command(&cmd_touch_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_touch_spec.run(argc, argv);
}
#endif
