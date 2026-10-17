/**
 * pwd command for AiShell.
 * Prints the current working directory using POSIX getcwd().
 *
 * Design notes:
 * - argtable3 drives parsing and usage; no manual argument handling.
 * - getcwd() retrieves the absolute pathname of the current working directory.
 * - Passing NULL as the buffer lets getcwd() allocate a buffer of the
 *   appropriate size automatically, which we free after printing.
 * - No -P/-L logical/physical distinctions are supported; this implementation
 *   always reports the physical path as returned by getcwd().
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/** argtable3 argument table for pwd.
 *  Layout: [help, end, NULL] — pwd takes no operands. */
typedef struct pwd_argtable {
    arg_lit_t *help;  /* -h/--help flag: prints usage and exits 0 */
    arg_end_t *end;   /* argtable3 terminator; collects parse errors */
    void *table[3];   /* Array of argtable3 pointers terminated by NULL */
} pwd_argtable_t;

/** Build the argtable3 table for pwd.
 *  Allocates the help literal and end marker.
 *  Returns 0 on success, -1 on allocation failure.
 *  The caller must free the table with arg_freetable(). */
static int build_pwd_argtable(pwd_argtable_t *args)
{
    /* Zero-initialize so partial failure does not leave dangling pointers. */
    *args = (pwd_argtable_t){0};

    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;

    args->end = arg_end(20);
    args->table[1] = args->end;

    /* NULL sentinel — arg_parse() traverses table until this terminator. */
    args->table[2] = NULL;

    if (args->help == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/** Print pwd usage derived from argtable3 definitions.
 *  Drives arg_print_syntax and arg_print_glossary so usage never
 *  diverges from the actual argument definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: pwd ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/** Public usage printer — called by AiShell's help system and standalone binaries. */
void pwd_print_usage(FILE *out)
{
    pwd_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_pwd_argtable(&args) != 0) {
        fputs("pwd: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Main execution entry point for the pwd command.
 *  Returns 0 on success, 1 on any error. */
int pwd_run(int argc, char **argv)
{
    pwd_argtable_t args;
    int nerrors;
    char *cwd;

    if (build_pwd_argtable(&args) != 0) {
        fputs("pwd: failed to allocate argument table\n", stderr);
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

    /* getcwd() returns the absolute pathname of the current working directory.
     * Passing NULL as the buffer lets getcwd() malloc() the correct size.
     * The returned pointer must be freed by the caller. */
    cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        /* getcwd() failed — errno is set (e.g., the current directory
         * was removed or is unreadable). */
        perror("pwd: getcwd");
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Print the working directory path followed by a newline. */
    if (printf("%s\n", cwd) < 0) {
        perror("pwd: stdout");
        free(cwd);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* free() releases the buffer that getcwd() allocated for us. */
    free(cwd);

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return 0;
}

cmd_spec_t cmd_pwd_spec = {
    "pwd",
    "print working directory",
    "Print the absolute pathname of the current working directory using getcwd(2).",
    pwd_run,
    pwd_print_usage
};

void register_pwd_command(void)
{
    register_command(&cmd_pwd_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_pwd_spec.run(argc, argv);
}
#endif