# AiShell

A modular C shell implementing standard Linux core utilities using the `cmd_spec_t` command architecture, argtable3 for argument parsing, and POSIX C99 APIs.

## Architecture

AiShell follows the command anatomy defined in [APPANATOMY.md](APPANATOMY.md):

- **`cmd_spec_t`** - The canonical command descriptor struct with `name`, `summary`, `long_help`, `run`, and `print_usage` fields.
- **Command modules** - Each command lives in `src/commands/cmd_<name>.c` and exports a single `cmd_spec_t` instance plus a `register_<name>_command()` helper.
- **Command registry** - `src/registry.c` provides a fixed-capacity registry for registering, looking up, and iterating commands.
- **Shell entry point** - `src/shell_main.c` supports three execution modes (see below).
- **argtable3** - Single-file amalgamation in `src/argtable3.c` provides all argument parsing and help generation.

## Execution Modes

`./aishell` supports three modes:

1. **Symlink multicall** - If invoked via a symlink whose basename matches a registered command (e.g., `./ls` or `./cat`), the command runs directly with `cmd->run(argc, argv)`.
2. **Subcommand multicall** - `./aishell <cmd> [args...]` dispatches a single command and exits.
3. **Interactive REPL** - `./aishell` with no arguments enters a read-eval-print loop that reads lines via `getline()`, tokenizes with `strtok_r()`, and dispatches commands in-process. Supports `exit` and `help [cmd]`.

## Available Commands

| Command | Description |
|---------|-------------|
| `cat`   | Concatenate and print files |
| `ls`    | List directory contents (`-a`, `-l`, `-h`, `--json`) |
| `stat`  | Display file or file system status (`--json`) |
| `head`  | Output the first part of files (`-n`) |
| `tail`  | Output the last part of files (`-n`) |
| `cp`    | Copy files and directories (`-r`) |
| `mv`    | Move/rename files and directories |
| `rm`    | Remove files and directories (`-r`) |
| `mkdir` | Create directories (`-p`) |
| `rmdir` | Remove empty directories |
| `touch` | Change file timestamps / create files |
| `pkg`   | Manage shell packages (install/list/info/remove) |

## Build

### Prerequisites

- A C99 compiler (tested with `gcc`)
- POSIX system headers

### Compiling

```sh
# Build the main aishell binary with all commands as built-in modules
make

# Build standalone command binaries (cmd_cat, cmd_ls, cmd_stat, etc.)
make standalone

# Clean build artifacts
make clean
```

### Dual Build Targets

Each command supports two compilation modes:

- **Built-in**: Compiled as an object file and linked into `aishell`, registered via the command registry.
- **Standalone**: Compiled with `-DSTANDALONE` to produce a `cmd_<name>` binary that can be invoked directly.

## Usage

```sh
# Interactive REPL mode
./aishell

# Subcommand mode
./aishell ls -l /tmp
./aishell cat -h

# Symlink multicall
ln -s aishell myls
./myls -l /tmp

# Standalone binaries
./cmd_ls -l /tmp
./cmd_cat sample.txt
```

### JSON Output

Agent-facing commands `ls` and `stat` support `--json` output:

```sh
./cmd_ls --json . | python3 -m json.tool
./cmd_stat --json sample.txt | python3 -m json.tool
```

## Testing

The project ships with a comprehensive verification script:

```sh
make clean && make
bash -n tests/verify_1.sh
./tests/verify_1.sh
```

The test suite (`tests/verify_1.sh`) runs integration tests across multiple stages covering build verification, command dispatch, help output, error handling, all command modules, `--json` validation, REPL mode, and symlink multicall.

## Project Layout

```
aishell/
├── APPANATOMY.md         # Command architecture specification
├── AGENTS.md             # Agent persona and workflow guidelines
├── ai_work_log.md        # Development work log
├── setup_test_pkg.sh        # Generates the mock cli-tools package tarball
├── Makefile              # Build system (built-in + standalone targets)
├── sample.txt            # Sample file for testing
├── include/              # Shared headers
├── src/
│   ├── argtable3.c       # argtable3 single-file amalgamation
│   ├── registry.c        # Command registry implementation
│   ├── shell_main.c      # Shell entry point (REPL, multicall)
│   └── commands/         # Command modules (cmd_<name>.c for each command)
├── tests/
│   └── verify_1.sh       # Integration test suite
```

## Documentation

- [APPANATOMY.md](APPANATOMY.md) - Command architecture specification
- [AGENTS.md](AGENTS.md) - Agent persona and development guidelines
- [ai_work_log.md](ai_work_log.md) - Development work log with prompts, decisions, and verification
