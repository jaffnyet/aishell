/**
 * cat command for AiShell.
 * Concatenates file contents to standard output, mimicking POSIX cat.
 *
 * Design notes:
 * - Argument parsing is handled entirely by argtable3, which builds an
 *   argument table that drives both the `run()` parser and the `print_usage`
 *   output generator. This ensures usage text never drifts from actual parsing.
 * - Files are read in 4KB chunks to handle files larger than any single buffer
 *   without loading entire contents into memory.
 * - On any error, cat stops processing immediately (POSIX-compliant behavior)
 *   and returns exit status 1.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <stdio.h>
#include <stdlib.h>

/** argtable3 argument table for cat.
 *  Layout: [help, files, end, NULL] — the end marker must be last. */
typedef struct cat_argtable {
    arg_lit_t *help;   /* -h/--help flag: prints usage and exits 0 */
    arg_file_t *files; /* One or more file operands, 1–100 required */
    arg_end_t *end;    /* argtable3 terminator; collects parse errors */
    void *table[4];    /* Array of argtable3 pointers terminated by NULL */
} cat_argtable_t;

/** Build the argtable3 table for cat.
 *  Allocates the help literal, file operand array, and end marker.
 *  Returns 0 on success, -1 on allocation failure.
 *  The caller must free the table with arg_freetable(). */
static int build_cat_argtable(cat_argtable_t *args)
{
    /* Keep the terminator last so Argtable3 can safely parse and free the table. */
    args->help = arg_litn("h", "help", 0, 1, "show usage and description");
    args->files = arg_filen(NULL, NULL, "<file>", 1, 100, "file(s) to print");
    args->end = arg_end(20);
    /* Array must be NULL-terminated for arg_parse() to traverse it safely. */
    args->table[0] = args->help;
    args->table[1] = args->files;
    args->table[2] = args->end;
    args->table[3] = NULL;

    if (args->help == NULL || args->files == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/** Generate and print usage text to the given stream.
 *  Drives arg_print_syntax and arg_print_glossary so the format never
 *  diverges from the parsed argument definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: cat ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/** Public usage printer — called by AiShell's help system and standalone binaries. */
void cat_print_usage(FILE *out)
{
    cat_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_cat_argtable(&args) != 0) {
        fputs("cat: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Main execution entry point for the cat command.
 *  Returns 0 on success, 1 on any error. */
int cat_run(int argc, char **argv)
{
    cat_argtable_t args;
    int nerrors;
    int status = 0;
    int i;
    char buffer[4096]; /* Chunk size for streaming file I/O */

    if (build_cat_argtable(&args) != 0) {
        fputs("cat: failed to allocate argument table\n", stderr);
        return 1;
    }

    /* Parse argv against the argtable3 definitions.
     * arg_parse returns the number of parse errors encountered. */
    nerrors = arg_parse(argc, argv, args.table);

    /* Help flag takes priority over syntax errors. */
    if (args.help->count > 0) {
        print_usage_with_argtable(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    /* On syntax errors, report to stderr and exit non-zero. */
    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Iterate over each file operand and stream its contents to stdout. */
    for (i = 0; i < args.files->count; ++i) {
        const char *filename = args.files->filename[i];

        /* fopen with "r" opens for reading; NULL return means failure (errno set). */
        FILE *file = fopen(filename, "r");
        if (file == NULL) {
            perror(filename); /* Prints "filename: error message" using errno */
            status = 1;
            break;
        }

        /* Copy in chunks to handle files larger than the input buffer. */
        for (;;) {
            /* fread reads up to sizeof(buffer) bytes; binary-safe due to size_t/1. */
            size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);

            /* Write the chunk to stdout. fwrite returns the number of items
             * written (each of size 1); if less than bytes_read, a write
             * error occurred. */
            if (bytes_read > 0 && fwrite(buffer, 1, bytes_read, stdout) != bytes_read) {
                perror("cat: stdout"); /* stdout write error */
                status = 1;
                break;
            }

            /* If we read fewer bytes than the buffer size, the file is exhausted. */
            if (bytes_read < sizeof(buffer)) {
                /* ferror checks if a read error occurred (as opposed to EOF). */
                if (ferror(file)) {
                    perror(filename);
                    status = 1;
                }
                break;
            }
        }

        /* fclose flushes and closes the file descriptor; failure may indicate
         * buffered data was not flushed or an I/O error occurred on close. */
        if (fclose(file) != 0) {
            perror(filename);
            status = 1;
        }

        if (status != 0) {
            break;
        }
    }

    /* Ensure all buffered output is flushed to the kernel before exiting. */
    if (status == 0 && fflush(stdout) == EOF) {
        perror("cat: stdout");
        status = 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_cat_spec = {
    "cat",
    "print file contents",
    "Concatenate FILE(s) to standard output.",
    cat_run,
    cat_print_usage
};

void register_cat_command(void)
{
    register_command(&cmd_cat_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_cat_spec.run(argc, argv);
}
#endif
