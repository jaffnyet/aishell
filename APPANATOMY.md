# AiShell Command Anatomy

This document defines the standard command architecture for AiShell, a Linux Systems Programming course project. It establishes the contract that all commands must follow to ensure consistency, maintainability, and dual compilation targets.

## 1. Target Structure

### 1.1 Command Specification (`cmd_spec_t`)

Every command in AiShell is defined by a single `cmd_spec_t` structure. This structure is the canonical description of a command and contains:

```c
typedef struct cmd_spec {
    const char *name;
    const char *summary;
    const char *long_help;
    int (*run)(int argc, char **argv);
    void (*print_usage)(FILE *out);
} cmd_spec_t;
```

**Fields:**

| Field         | Description                                                                 |
|---------------|-----------------------------------------------------------------------------|
| `name`        | The canonical command name as invoked on the shell (e.g., `"cd"`, `"ls"`).  |
| `summary`     | A one-line description (max 80 characters), shown in command listings.        |
| `long_help`   | Detailed help text providing extended usage information and examples.       |
| `run`         | The primary entry point. Receives `argc`/`argv` after option parsing.       |
| `print_usage` | Function pointer to print full usage text to the given `FILE *out`.         |

### 1.2 Module Export Contract

Each command module must export:

1. Exactly one `cmd_spec_t` instance named `cmd_<name>_spec` (e.g., `cmd_cd_spec`).
2. A registration helper function with the signature:
   ```c
   void register_<cmd>_command(void);
   ```

**Example:**

```c
// File: src/commands/cd.c
#include "cmd_spec.h"

static cmd_spec_t cmd_cd_spec = {
    .name = "cd",
    .summary = "Change the current working directory",
    .long_help = "...",
    .run = cmd_cd_run,
    .print_usage = cmd_cd_print_usage,
};

void register_cd_command(void) {
    command_registry_register(&cmd_cd_spec);
}
```

## 2. argtable3 as Single Source of Truth

The `cmd_spec_t` structure and argtable3 parsing definitions form the single source of truth for all command behavior. No command may have separate parsing logic for its `run()` and `print_usage()` functions.

### 2.1 Parsing and Usage Derivation

- All argument parsing must be performed using the argtable3 library (`argtable3.h`).
- The `print_usage` function must generate its output directly from the argtable3 argument table definitions using `arg_print_errors` and `arg_print_help`.
- No separate help text strings or manual formatting should exist outside the argtable3 definitions.

### 2.2 Standard Help Handling

- `-h` and `--help` flags must print the full usage/help text to `stdout` and exit with status `0`.
- The help text must match exactly what `arg_print_help` produces from the command's argtable3 definitions.

**Example:**

```c
static void cmd_cd_print_usage(FILE *out) {
    struct arg_lit *help = arg_litn("h", "help", 0, 1);
    // ... other argtable3 definitions ...
    struct arg_end *end = arg_end(20);
    void *argtable[] = {help, /* ... */, end};

    if (out == stdout) {
        arg_print_help(argtable, out);
    } else {
        // Handle stderr case if needed
    }

    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
}
```

### 2.3 Syntax Error Handling

- Any parsing error (invalid flag, missing required argument, unknown option) must print an error message to `stderr`.
- After printing the error, the usage/help text must also be printed to `stderr`.
- The program must exit with a non-zero exit code (typically `1`).

**Example:**

```c
static int cmd_cd_run(int argc, char **argv) {
    // argtable3 setup...
    struct arg_end *end = arg_end(20);

    int nerrors = arg_parse(argc, argv, argtable);
    if (nerrors > 0) {
        arg_print_errors(stderr, end, argv[0]);
        arg_print_help(argtable, stderr);
        // Cleanup and exit
        return 1;
    }

    // Normal execution...
    return 0;
}
```

## 3. Dual Build Targets

Each command module must support compilation in two distinct modes:

### 3.1 Built-in Command

- Compiled as a shared object (`.so`) or static object (`.o`) and linked into the main AiShell binary.
- Registered with the AiShell command registry via the `register_<cmd>_command()` function.
- Available as an internal command when AiShell is running.

### 3.2 Standalone Binary

- A separate compilation target produces a standalone executable named `cmd_<name>`.
- The executable contains a minimal `main()` function that calls `cmd_<name>_spec.run(argc, argv)`.
- This allows individual command testing and direct invocation outside of AiShell.

**Example Makefile rules:**

```makefile
# Built-in module
commands/%.o: commands/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Standalone binary
cmd_%: commands/%.o cmd_main.o
	$(CC) $(CFLAGS) $< cmd_main.o -o $@ -largtable3
```

**Example `cmd_main.o` (shared wrapper):**

```c
extern cmd_spec_t cmd_<name>_spec;

int main(int argc, char **argv) {
    return cmd_<name>_spec.run(argc, argv);
}
```

## 4. Refactoring Contract

When migrating legacy commands to the `cmd_spec_t` architecture, the following contract must be followed:

### 4.1 Extract Argument Parsing

1. Identify all command-line argument parsing logic in the legacy command.
2. Replace it with argtable3 definitions (flags, options, positional arguments).
3. Ensure the argtable3 definitions capture all parsing rules from the legacy implementation.
4. Use `arg_parse()` to process arguments consistently.

### 4.2 Isolate Business Logic

1. Move all command business logic (the actual work the command performs) into the `run()` function.
2. The `run()` function should only contain core logic after successful argument parsing.
3. Argument parsing and validation should happen at the top of `run()`, before any business logic.
4. If the legacy command contains sub-functions, they should remain as private static functions within the module.

### 4.3 Maintain CLI Behavior

1. **Exit statuses**: The refactored command must exit with the same status codes as the original for all success and failure cases.
2. **Output format**: stdout and stderr output must match exactly, byte-for-byte, under all conditions (normal operation, errors, help, and version output).
3. **Argument compatibility**: All existing command-line invocations must continue to work without modification. No breaking changes to the CLI interface are permitted.
4. **Help text**: If the legacy command had help text, it should be preserved in the `long_help` field and `print_usage` output. The usage section should be generated from argtable3.

### 4.4 Verification Checklist

Before refactoring is considered complete, the following must be verified:

- [ ] `cmd_spec_t` is defined with all fields populated correctly.
- [ ] `register_<cmd>_command()` function exists and registers the command.
- [ ] argtable3 is used for all argument parsing in `run()`.
- [ ] `-h`/`--help` prints to `stdout` and exits with `0`.
- [ ] Invalid arguments print errors to `stderr` and exit non-zero.
- [ ] Built-in compilation produces a properly registered module.
- [ ] Standalone binary compiles and passes all existing behavioral tests.
- [ ] Exit statuses match legacy behavior for all test cases.
- [ ] stdout/stderr output matches legacy behavior byte-for-byte.

## 5. File Organization

Each command should follow this file layout:

```
src/commands/<name>/
├── cmd_<name>.c     # Main command implementation
├── cmd_<name>.h     # Header with any exports
├── cmd_<name>.o     # Compiled object (built-in)
└── cmd_<name>       # Standalone binary (standalone)
```

Or for simpler commands, a flat layout may be used:

```
src/commands/
├── cmd_<name>.c
├── cmd_<name>.h
├── cmd_<name>.o
└── cmd_<name>
```

## 6. Global Definitions

The canonical `cmd_spec_t` definition should be placed in a shared header:

```c
// include/cmd_spec.h
#ifndef CMD_SPEC_H
#define CMD_SPEC_H

#include <stdio.h>

typedef struct cmd_spec {
    const char *name;
    const char *summary;
    const char *long_help;
    int (*run)(int argc, char **argv);
    void (*print_usage)(FILE *out);
} cmd_spec_t;

void command_registry_register(cmd_spec_t *spec);

#endif /* CMD_SPEC_H */
```
