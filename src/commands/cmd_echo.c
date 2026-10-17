/**
 * echo command for AiShell.
 * Writes its arguments (text strings) to standard output, separated by
 * spaces and terminated by a newline.
 *
 * Design notes:
 * - argtable3 drives parsing and usage; no manual argument handling.
 * - Supports a -n flag (do not output a trailing newline), matching
 *   the POSIX sh echo semantic.
 * - Text operands are captured via arg_filen() (the available argtable3
 *   positional type in this build). The filename[] array holds the
 *   text strings; we print them space-separated.
 * - No file operands are expected; all non-option arguments are
 *   treated as text to print.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** argtable3 argument table for echo.
 *  Layout: [help, newline, text, end, NULL]
 *  echo has no flags of its own beyond -n and -h. */
typedef struct echo_argtable {
    arg_lit_t *help;    /* -h/--help flag: prints usage and exits 0 */
    arg_lit_t *newline; /* -n: suppress trailing newline */
    arg_file_t *text;   /* Zero or more positional text operands */
    arg_end_t *end;     /* argtable3 terminator; collects parse errors */
    void *table[5];     /* Array of argtable3 pointers terminated by NULL */
} echo_argtable_t;

/** Build the argtable3 table for echo.
 *  Allocates the help literal, the -n flag, the text operand array,
 *  and the end marker.
 *  Returns 0 on success, -1 on allocation failure.
 *  The caller must free the table with arg_freetable(). */
static int build_echo_argtable(echo_argtable_t *args)
{
    /* Zero-initialize so partial failure does not leave dangling pointers. */
    *args = (echo_argtable_t){0};

    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;

    args->newline = arg_lit0("n", NULL, "do not output the trailing newline");
    args->table[1] = args->newline;

    /* arg_filen with mincount=0, maxcount=100 captures 0–100 positional
     * text operands. The filename[] array within arg_file will hold
     * pointers to the text strings from argv. */
    args->text = arg_filen(NULL, NULL, "<text>", 0, 100, "text to print");
    args->table[2] = args->text;

    args->end = arg_end(20);
    args->table[3] = args->end;

    /* NULL sentinel — arg_parse() traverses table until this terminator. */
    args->table[4] = NULL;

    if (args->help == NULL || args->newline == NULL ||
        args->text == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/** Print echo usage derived from argtable3 definitions.
 *  Drives arg_print_syntax and arg_print_glossary so usage never
 *  diverges from the actual argument definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: echo ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/** Public usage printer — called by AiShell's help system and standalone binaries. */
void echo_print_usage(FILE *out)
{
    echo_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_echo_argtable(&args) != 0) {
        fputs("echo: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Main execution entry point for the echo command.
 *  Returns 0 on success, 1 on any error. */
int echo_run(int argc, char **argv)
{
    echo_argtable_t args;
    int nerrors;
    int i;

    if (build_echo_argtable(&args) != 0) {
        fputs("echo: failed to allocate argument table\n", stderr);
        return 1;
    }

    /* arg_parse() processes argv against the argtable3 definitions;
     * returns the count of parse errors encountered. */
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

    /* Echo prints each text operand separated by spaces.
     * argtable3 has populated args.text->filename[] with the positional
     * tokens; args.text->count gives the number of such tokens. */
    for (i = 0; i < args.text->count; ++i) {
        /* Print a space separator before every argument except the first. */
        if (i > 0) {
            fputc(' ', stdout);
        }
        fputs(args.text->filename[i], stdout);
    }

    /* Print trailing newline unless -n was specified. */
    if (args.newline->count == 0) {
        fputc('\n', stdout);
    }

    /* Flush stdout and check for write errors (e.g., broken pipe). */
    if (fflush(stdout) == EOF) {
        perror("echo: stdout");
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return 0;
}

cmd_spec_t cmd_echo_spec = {
    "echo",
    "display a line of text",
    "Write the given text arguments to standard output, separated by spaces. A trailing newline is printed unless -n is given.",
    echo_run,
    echo_print_usage
};

void register_echo_command(void)
{
    register_command(&cmd_echo_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_echo_spec.run(argc, argv);
}
#endif