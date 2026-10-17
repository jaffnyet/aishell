/**
 * head command for AiShell.
 * Prints the first N lines of each input file (default: 10 lines).
 *
 * Design notes:
 * - argtable3 drives parsing and usage; the -n option accepts an integer
 *   argument via arg_int0.
 * - Uses getline() for line reading, which handles arbitrarily long lines
 *   without fixed buffer limits.
 * - Output is flushed at the end; write errors are checked throughout.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <stdio.h>
#include <stdlib.h>

/** argtable3 argument table for head.
 *  Layout: [help, lines, files, end, NULL] */
typedef struct head_argtable {
    arg_lit_t *help;   /* -h/--help flag */
    arg_int_t *lines;  /* -n/--lines: number of lines to print (default 10) */
    arg_file_t *files; /* One or more file operands */
    arg_end_t *end;    /* argtable3 error terminator */
    void *table[5];
} head_argtable_t;

/** Build the argtable3 table for head.
 *  Sets the default line count to 10 via lines->ival[0] initialization.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_head_argtable(head_argtable_t *args)
{
    *args = (head_argtable_t){0};
    args->help = arg_litn("h", "help", 0, 1, "show usage and description");
    args->lines = arg_int0("n", "lines", "<n>", "number of lines to print");
    args->files = arg_filen(NULL, NULL, "<file>", 1, 100, "file(s) to print");
    args->end = arg_end(20);
    args->table[0] = args->help;
    args->table[1] = args->lines;
    args->table[2] = args->files;
    args->table[3] = args->end;
    args->table[4] = NULL;

    if (args->help == NULL || args->lines == NULL || args->files == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    /* Set the default value for the -n option: 10 lines. */
    args->lines->ival[0] = 10;
    return 0;
}

/** Print head usage derived from argtable3 definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: head ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void head_print_usage(FILE *out)
{
    head_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_head_argtable(&args) != 0) {
        fputs("head: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Read and print the first line_count lines from a file.
 *  Uses getline() which allocates/reallocates the line buffer as needed.
 *  Returns 0 on success, 1 on error. */
static int print_head_file(const char *path, int line_count)
{
    FILE *file;
    char *line = NULL;   /* getline() manages this buffer; we free it. */
    size_t capacity = 0; /* Buffer capacity (set by getline) */
    ssize_t chars_read;  /* Number of characters read (including '\n') */
    int printed = 0;
    int failed = 0;

    /* fopen() with "r" opens for reading in text mode.
     * Returns NULL with errno set if the file cannot be opened. */
    file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return 1;
    }

    /* getline() reads a delimited line; handles arbitrary line lengths.
     * Returns -1 on EOF or error; the line buffer is reused across calls. */
    while (printed < line_count &&
           (chars_read = getline(&line, &capacity, file)) != -1) {
        /* fwrite() writes the exact bytes read from the file to stdout.
         * Returns the number of items written; mismatch indicates a write error. */
        if (fwrite(line, 1, (size_t)chars_read, stdout) != (size_t)chars_read) {
            perror("head: stdout");
            failed = 1;
            break;
        }
        ++printed;
    }

    /* ferror() distinguishes EOF from a read error on the file stream. */
    if (ferror(file)) {
        perror(path);
        failed = 1;
    }

    /* Free the line buffer allocated by getline(). */
    free(line);

    /* fclose() flushes and closes the stream; returns EOF on error. */
    if (fclose(file) != 0) {
        perror(path);
        failed = 1;
    }

    return failed ? 1 : 0;
}

int head_run(int argc, char **argv)
{
    head_argtable_t args;
    int nerrors;
    int status = 0;
    int i;
    int line_count;

    if (build_head_argtable(&args) != 0) {
        fputs("head: failed to allocate argument table\n", stderr);
        return 1;
    }

    nerrors = arg_parse(argc, argv, args.table);

    if (args.help->count > 0) {
        print_usage_with_argtable(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Reject negative line counts as invalid. */
    if (args.lines->count > 0 && args.lines->ival[0] < 0) {
        fprintf(stderr, "%s: invalid line count: '%d'\n", argv[0], args.lines->ival[0]);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Use the user-specified count, or the default (10) if -n was not given. */
    line_count = (args.lines->count > 0) ? args.lines->ival[0] : 10;

    for (i = 0; i < args.files->count; ++i) {
        if (print_head_file(args.files->filename[i], line_count) != 0) {
            status = 1;
            break;
        }
    }

    /* Flush stdout to detect write errors (e.g., broken pipe). */
    if (fflush(stdout) == EOF) {
        perror("head: stdout");
        status = 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_head_spec = {
    "head",
    "output the first part of files",
    "Print the first N lines of each FILE to standard output.",
    head_run,
    head_print_usage
};

void register_head_command(void)
{
    register_command(&cmd_head_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_head_spec.run(argc, argv);
}
#endif
