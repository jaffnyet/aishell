#!/usr/bin/env bash
#
# verify_pkg.sh
#
# Comprehensive verification of the AiShell `pkg` package-manager command.
# Demonstrates the complete package lifecycle (build, install, list, info,
# execute all binaries, remove, cleanup verification) for the Week 4
# assignment using the multi-binary cli-tools package.
#
# Uses an isolated HOME so the real ~/.mysh is never touched.
# Exits immediately on unexpected errors (set -euo pipefail).
# Prints clear green/red status indicators for readable demo evidence.

set -euo pipefail

# ------------------------------------------------------------------ #
# Configuration                                                       #
# ------------------------------------------------------------------ #

# Resolve the project root (directory containing this script).
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
cd "$PROJECT_ROOT"

# Isolated HOME so pkg tests never touch the real ~/.mysh.
PKG_TEST_HOME="$(mktemp -d tests/pkg-demo-home.XXXXXX)"
export HOME="$PKG_TEST_HOME"

# Package identifiers.
PKG_NAME="cli-tools"
PKG_VERSION="0.1.0"
PKG_TARBALL="$PKG_NAME-$PKG_VERSION.tar.gz"
PKG_DIR="$PKG_NAME-$PKG_VERSION"

# The binaries shipped by the cli-tools package.
PKG_BINARIES=("cat" "head" "stat")

# ANSI color codes for readable status output.
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# Track overall pass/fail status.
FAILURES=0

# ------------------------------------------------------------------ #
# Helper functions                                                    #
# ------------------------------------------------------------------ #

# Print a green PASS line.
pass()
{
    printf "${GREEN}[PASS]${NC} %s\n" "$1"
}

# Print a red FAIL line and increment the failure counter.
fail()
{
    printf "${RED}[FAIL]${NC} %s\n" "$1"
    FAILURES=$((FAILURES + 1))
}

# Print a yellow step indicator.
step()
{
    printf "${YELLOW}=== %s ===${NC}\n" "$1"
}

# Run an assertion: check that a command succeeds (exit 0).
# Usage: assert_ok "description" command...
assert_ok()
{
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        pass "$desc"
    else
        fail "$desc"
    fi
}

# Run an assertion: check that a command's output contains a substring.
# Usage: assert_contains "description" "substring" command...
# Captures output to a temp file (not a pipe) to avoid SIGPIPE under pipefail.
assert_contains()
{
    local desc="$1"
    local substring="$2"
    shift 2
    local tmp
    tmp="$(mktemp)"
    if "$@" >"$tmp" 2>&1; then
        if grep -Fq "$substring" "$tmp"; then
            pass "$desc"
        else
            fail "$desc (expected: '$substring')"
            cat "$tmp" >&2
        fi
    else
        fail "$desc (command failed)"
        cat "$tmp" >&2
    fi
    rm -f "$tmp"
}

# Run an assertion: check that a command's output contains a substring,
# regardless of exit status. Used for commands expected to fail but
# still produce diagnostic output.
# Usage: assert_contains_anyexit "description" "substring" command...
assert_contains_anyexit()
{
    local desc="$1"
    local substring="$2"
    shift 2
    local tmp
    tmp="$(mktemp)"
    "$@" >"$tmp" 2>&1 || true
    if grep -Fq "$substring" "$tmp"; then
        pass "$desc"
    else
        fail "$desc (expected: '$substring')"
        cat "$tmp" >&2
    fi
    rm -f "$tmp"
}

# Clean up the isolated HOME on exit.
cleanup()
{
    rm -rf -- "$PKG_TEST_HOME"
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# Step 1: Clean rebuild                                               #
# ------------------------------------------------------------------ #

step "Step 1: Clean rebuild (make clean && make)"

if make clean >/dev/null 2>&1; then
    pass "make clean"
else
    fail "make clean"
    exit 1
fi

if make >/dev/null 2>&1; then
    pass "make (aishell built)"
else
    fail "make"
    exit 1
fi

if [[ -x ./aishell ]]; then
    pass "aishell binary exists and is executable"
else
    fail "aishell binary not found or not executable"
    exit 1
fi

# ------------------------------------------------------------------ #
# Step 2: Command discovery via help                                 #
# ------------------------------------------------------------------ #

step "Step 2: Verify command discovery (help)"

assert_contains "pkg appears in help output" "pkg" \
    ./aishell help

if ./aishell help pkg > /dev/null 2>&1; then
    pass "pkg subcommand documentation displays"
else
    fail "pkg subcommand documentation did not display"
fi

# ------------------------------------------------------------------ #
# Step 3: Prepare the mock package tarball                           #
# ------------------------------------------------------------------ #

step "Step 3: Generate mock package (cli-tools)"

if ./setup_test_pkg.sh > /tmp/setup_pkg.log 2>&1; then
    pass "setup_test_pkg.sh generated $PKG_TARBALL"
else
    fail "setup_test_pkg.sh failed"
    cat /tmp/setup_pkg.log >&2
    exit 1
fi

if [[ -f "$PKG_TARBALL" ]]; then
    pass "Mock package tarball present: $PKG_TARBALL"
else
    fail "Mock package tarball missing"
    exit 1
fi

# ------------------------------------------------------------------ #
# Step 4: Install the package                                        #
# ------------------------------------------------------------------ #

step "Step 4: Install package"

assert_ok "pkg install exits 0" ./aishell pkg install "./$PKG_TARBALL"

assert_contains "install reports success" \
    "installed successfully" \
    ./aishell pkg install "./$PKG_TARBALL"

if [[ -d "$HOME/.mysh/pkgs/$PKG_DIR" ]]; then
    pass "Package directory created at ~/.mysh/pkgs/$PKG_DIR"
else
    fail "Package directory not found at ~/.mysh/pkgs/$PKG_DIR"
fi

if [[ -f "$HOME/.mysh/pkgdb.txt" ]]; then
    pass "Package database created at ~/.mysh/pkgdb.txt"
else
    fail "Package database not found at ~/.mysh/pkgdb.txt"
fi

# Verify ALL three binaries are installed and executable.
for bin in "${PKG_BINARIES[@]}"; do
    if [[ -x "$HOME/.mysh/bin/$bin" ]]; then
        pass "Binary installed and executable: ~/.mysh/bin/$bin"
    else
        fail "Binary not found or not executable: ~/.mysh/bin/$bin"
    fi
done

# ------------------------------------------------------------------ #
# Step 5: List installed packages                                    #
# ------------------------------------------------------------------ #

step "Step 5: List installed packages"

assert_contains "pkg list shows $PKG_DIR" \
    "$PKG_DIR" \
    ./aishell pkg list

# ------------------------------------------------------------------ #
# Step 6: Info about the package                                     #
# ------------------------------------------------------------------ #

step "Step 6: Package info"

assert_contains "pkg info shows Name" "Name:    $PKG_NAME" \
    ./aishell pkg info "$PKG_NAME"
assert_contains "pkg info shows Version" "Version: $PKG_VERSION" \
    ./aishell pkg info "$PKG_NAME"
assert_contains "pkg info shows Desc" "Desc:    Core command-line utilities bundle for aishell" \
    ./aishell pkg info "$PKG_NAME"
assert_contains "pkg info shows Files section" "Files:" \
    ./aishell pkg info "$PKG_NAME"

# Verify info lists all three binary files.
assert_contains "pkg info lists bin/cat" "bin/cat" \
    ./aishell pkg info "$PKG_NAME"
assert_contains "pkg info lists bin/head" "bin/head" \
    ./aishell pkg info "$PKG_NAME"
assert_contains "pkg info lists bin/stat" "bin/stat" \
    ./aishell pkg info "$PKG_NAME"

# ------------------------------------------------------------------ #
# Step 7: Execute all installed binaries                            #
# ------------------------------------------------------------------ #

step "Step 7: Execute installed binaries"

# Run each installed binary against the project Makefile to confirm
# they are functional. Output is captured to a temp file (not piped)
# to avoid SIGPIPE under pipefail.
for bin in "${PKG_BINARIES[@]}"; do
    tmp_out="$(mktemp)"
    if "$HOME/.mysh/bin/$bin" Makefile > "$tmp_out" 2>&1; then
        pass "Installed $bin binary executes successfully"
    else
        fail "Installed $bin binary exited non-zero"
        cat "$tmp_out" >&2
    fi
    rm -f "$tmp_out"
done

# ------------------------------------------------------------------ #
# Step 8: Remove the package                                         #
# ------------------------------------------------------------------ #

step "Step 8: Remove package"

assert_ok "pkg remove exits 0" ./aishell pkg remove "$PKG_NAME"

# Verify ALL three binaries are deleted from ~/.mysh/bin.
for bin in "${PKG_BINARIES[@]}"; do
    if [[ ! -e "$HOME/.mysh/bin/$bin" ]]; then
        pass "Binary removed from ~/.mysh/bin/$bin"
    else
        fail "Binary still exists at ~/.mysh/bin/$bin"
    fi
done

if [[ ! -d "$HOME/.mysh/pkgs/$PKG_DIR" ]]; then
    pass "Package directory removed: ~/.mysh/pkgs/$PKG_DIR"
else
    fail "Package directory still exists: ~/.mysh/pkgs/$PKG_DIR"
fi

# ------------------------------------------------------------------ #
# Step 9: Verify list is empty after removal                         #
# ------------------------------------------------------------------ #

step "Step 9: Verify cleanup"

assert_contains "pkg list reports no packages after remove" \
    "No packages installed." \
    ./aishell pkg list

assert_contains_anyexit "pkg info returns not-installed error" \
    "package not installed: $PKG_NAME" \
    ./aishell pkg info "$PKG_NAME"

# ------------------------------------------------------------------ #
# Summary                                                             #
# ------------------------------------------------------------------ #

step "Summary"

if [[ "$FAILURES" -eq 0 ]]; then
    printf "${GREEN}========================================${NC}\n"
    printf "${GREEN}All verification checks PASSED.${NC}\n"
    printf "${GREEN}========================================${NC}\n"
    exit 0
else
    printf "${RED}========================================${NC}\n"
    printf "${RED}%d check(s) FAILED.${NC}\n" "$FAILURES"
    printf "${RED}========================================${NC}\n"
    exit 1
fi
