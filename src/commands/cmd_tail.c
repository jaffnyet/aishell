/**
 * tail command for AiShell.
 * Prints the last N lines of each input file (default: 10 lines).
 *
 * Design notes:
 * - argtable3 drives parsing and usage; the -n option accepts an integer.
 * - Uses a circular buffer (ring buffer) of line strings to efficiently
 *   retain only the last N lines without storing the entire file in memory.
 * - Uses getline() for line reading, handling arbitrarily long lines.
 * - The circular buffer is implemented with a char** array and a `next`
 *   index that wraps around via modulo arithmetic.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** argtable3 argument table for tail.
 *  Layout: [help, lines, files, end, NULL] */
typedef struct tail_argtable {
    arg_lit_t *help;   /* -h/--help flag */
    arg_int_t *lines;  /* -n/--lines: number of lines to print (default 10) */
    arg_file_t *files; /* One or more file operands */
    arg_end_t *end;    /* argtable3 error terminator */
    void *table[5];
} tail_argtable_t;

/** Build the argtable3 table for tail.
 *  Sets the default line count to 10 via lines->ival[0] initialization.
 *  Returns 0 on success, -1 on allocation failure. */
static int build_tail_argtable(tail_argtable_t *args)
{
    *args = (tail_argtable_t){0};
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

/** Print tail usage derived from argtable3 definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: tail ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

void tail_print_usage(FILE *out)
{
    tail_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_tail_argtable(&args) != 0) {
        fputs("tail: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Duplicate a string, returning a malloc'd copy or NULL on allocation failure.
 *  Used to store line copies in the circular buffer since getline's
 *  internal buffer is reused on each call. */
static char *duplicate_string(const char *input)
{
    size_t length;
    char *copy;

    if (input == NULL) {
        return NULL;
    }

    length = strlen(input);
    copy = malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, input, length + 1U);
    return copy;
}

/** Print the last line_count lines from a file.
 *  Uses a ring buffer that holds exactly `limit` line strings, overwriting
 *  the oldest entries as new lines arrive. After reading, the buffer
 *  is traversed starting from the `next` index to print in order. */
static int print_tail_file(const char *path, int line_count)
{
    FILE *file;
    char **buffer = NULL;  /* Ring buffer of line strings */
    char *line = NULL;     /* getline() manages this buffer */
    size_t buffer_capacity = 0;
    size_t line_capacity = 0;
    size_t stored = 0;     /* Number of lines currently in the buffer */
    size_t limit = (size_t)line_count;
    size_t next = 0;       /* Index of the oldest line (wraps around) */
    size_t i;
    ssize_t chars_read;
    int failed = 0;

    /* fopen() opens the file for reading; NULL on failure (errno set). */
    file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return 1;
    }

    /* Special case: -n 0 means print nothing. */
    if (limit == 0) {
        /* fclose() flushes and releases the file descriptor. */
        if (fclose(file) != 0) {
            perror(path);
            return 1;
        }
        return 0;
    }

    /* Initialize the ring buffer with a small capacity (min 16 for efficiency). */
    buffer_capacity = limit < 16 ? limit : 16;
    /* calloc() zero-initializes the array of char pointers. */
    buffer = calloc(buffer_capacity, sizeof(*buffer));
    if (buffer == NULL) {
        fprintf(stderr, "tail: failed to allocate line buffer\n");
        fclose(file);
        return 1;
    }

    /* getline() reads one line at a time, growing the buffer as needed.
     * Returns -1 at EOF or on error. */
    while ((chars_read = getline(&line, &line_capacity, file)) != -1) {
        /* Duplicate the line because getline reuses its buffer on each call. */
        char *copy = duplicate_string(line);
        if (copy == NULL) {
            fprintf(stderr, "tail: failed to allocate line storage\n");
            failed = 1;
            free(line);
            /* Clean up all buffered lines before exiting. */
            for (i = 0; i < stored; ++i) {
                free(buffer[i]);
            }
            free(buffer);
            fclose(file);
            return 1;
        }

        if (stored < limit) {
            /* Buffer not yet full: grow it if needed. */
            if (stored == buffer_capacity) {
                /* Double the capacity (or jump to limit, whichever is smaller). */
                size_t new_capacity = buffer_capacity > limit / 2
                                      ? limit
                                      : buffer_capacity * 2;

                /* Check for potential overflow before multiplying. */
                if (new_capacity > (size_t)-1 / sizeof(*buffer)) {
                    fprintf(stderr, "tail: line buffer size is too large\n");
                    free(copy);
                    free(line);
                    for (i = 0; i < stored; ++i) {
                        free(buffer[i]);
                    }
                    free(buffer);
                    fclose(file);
                    return 1;
                }
                /* realloc() grows the array preserving existing contents. */
                char **resized = realloc(buffer, new_capacity * sizeof(*buffer));
                if (resized == NULL) {
                    fprintf(stderr, "tail: failed to grow line buffer\n");
                    free(copy);
                    free(line);
                    for (i = 0; i < stored; ++i) {
                        free(buffer[i]);
                    }
                    free(buffer);
                    fclose(file);
                    return 1;
                }
                buffer = resized;
                buffer_capacity = new_capacity;
            }
            buffer[stored++] = copy;
        } else {
            /* Buffer is full: overwrite the oldest line and advance the ring. */
            free(buffer[next]);
            buffer[next] = copy;
            next = (next + 1) % limit;
        }
    }

    /* Free the getline-managed buffer (no longer needed after the read loop). */
    free(line);

    /* Check if the loop ended due to a read error (not just EOF). */
    if (ferror(file)) {
        perror(path);
        failed = 1;
    }

    /* Print the ring buffer contents in chronological order.
     * If stored == limit, the buffer is full and we start from `next`.
     * Otherwise, entries are stored linearly and we print from index 0. */
    for (i = 0; i < stored; ++i) {
        size_t index = stored == limit ? (next + i) % stored : i;

        /* fputs() writes a null-terminated string to stdout.
         * Returns EOF on error (e.g., broken pipe). */
        if (fputs(buffer[index], stdout) == EOF) {
            perror("tail: stdout");
            failed = 1;
            break;
        }
    }

    /* Free each line string in the buffer, then the buffer itself. */
    for (i = 0; i < stored; ++i) {
        free(buffer[i]);
    }
    free(buffer);

    /* fclose() flushes buffered writes and closes the file descriptor. */
    if (fclose(file) != 0) {
        perror(path);
        failed = 1;
    }

    return failed ? 1 : 0;
}

int tail_run(int argc, char **argv)
{
    tail_argtable_t args;
    int nerrors;
    int status = 0;
    int i;
    int line_count;

    if (build_tail_argtable(&args) != 0) {
        fputs("tail: failed to allocate argument table\n", stderr);
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

    line_count = (args.lines->count > 0) ? args.lines->ival[0] : 10;

    for (i = 0; i < args.files->count; ++i) {
        if (print_tail_file(args.files->filename[i], line_count) != 0) {
            status = 1;
            break;
        }
    }

    /* Final flush to detect any pending write errors on stdout. */
    if (fflush(stdout) == EOF) {
        perror("tail: stdout");
        status = 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return status;
}

cmd_spec_t cmd_tail_spec = {
    "tail",
    "output the last part of files",
    "Print the last N lines of each FILE to standard output.",
    tail_run,
    tail_print_usage
};

void register_tail_command(void)
{
    register_command(&cmd_tail_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_tail_spec.run(argc, argv);
}
#endif
