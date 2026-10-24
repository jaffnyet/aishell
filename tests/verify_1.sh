#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_COMPLETE=0
LS_TEST_DIR=""
COMMAND_TEST_DIR=""
SYMLINK_DIR=""
PKG_TEST_HOME=""

cleanup()
{
    rm -f -- \
        test.txt \
        tests/test_cat_error.c \
        tests/test_cat_error \
        tests/test_cat_error.log \
        tests/aishell_test.log \
        tests/cmd_symlink_ls \
        cli-tools-0.1.0.tar.gz
    rm -rf -- fixtures/cli-tools

    if [[ -n "$LS_TEST_DIR" ]]; then
        rm -rf -- "$LS_TEST_DIR"
    fi
    if [[ -n "$COMMAND_TEST_DIR" ]]; then
        rm -rf -- "$COMMAND_TEST_DIR"
    fi
    if [[ -n "$SYMLINK_DIR" ]]; then
        rm -rf -- "$SYMLINK_DIR"
    fi

    if [[ "$BUILD_COMPLETE" -eq 0 ]]; then
        make clean >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

echo "[Stage 1/7] Build verification"
if ! make clean; then
    echo "[FAIL] make clean failed." >&2
    exit 1
fi
if ! make; then
    echo "[FAIL] make failed; aishell was not built." >&2
    exit 1
fi
if [[ ! -x ./aishell ]]; then
    echo "[FAIL] make completed without producing an executable aishell." >&2
    exit 1
fi
echo "[PASS] aishell was built successfully."
BUILD_COMPLETE=1

echo "[Stage 2/7] Happy-path verification"
printf 'AiShell cat test sample.\n' > test.txt
if ./aishell cat test.txt > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx 'AiShell cat test sample.' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] cat did not print the expected file contents." >&2
        exit 1
    fi
    echo "[PASS] ./aishell cat test.txt printed the expected contents."
else
    status=$?
    cat tests/aishell_test.log
    echo "[FAIL] ./aishell cat test.txt exited with status $status." >&2
    exit 1
fi
if ./aishell cat -h > tests/aishell_test.log 2>&1; then
    if ! grep -Fq 'Usage: cat' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] cat help output did not contain usage." >&2
        exit 1
    fi
    echo "[PASS] ./aishell cat -h displayed command usage."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell cat -h exited with status $status." >&2
    exit 1
fi

if ./aishell < /dev/null > tests/aishell_test.log 2>&1; then
    if ! grep -Fq 'cat        - print file contents' tests/aishell_test.log ||
       ! grep -Fq 'ls         - list directory contents' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] Running aishell without a command did not list all commands." >&2
        exit 1
    fi
    echo "[PASS] ./aishell lists all registered commands."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell exited with status $status." >&2
    exit 1
fi

if ./aishell help cat > tests/aishell_test.log 2>&1; then
    if ! grep -Fq 'cat - print file contents' tests/aishell_test.log ||
       ! grep -Fq 'Concatenate FILE(s) to standard output.' tests/aishell_test.log ||
       ! grep -Fq 'Usage: cat' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] help cat omitted summary, description, or usage." >&2
        exit 1
    fi
    echo "[PASS] ./aishell help cat displays summary, description, and usage."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell help cat exited with status $status." >&2
    exit 1
fi

if ./aishell help does-not-exist > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] help for an unknown command unexpectedly succeeded." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]] ||
   ! grep -Fqx 'aishell: command not found: does-not-exist' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] Unknown-command help returned status $status or unexpected output." >&2
    exit 1
fi
echo "[PASS] Unknown-command help reports the missing command and exits with status 1."

echo "[Stage 3/7] ls command verification"
LS_TEST_DIR="$(mktemp -d tests/ls-fixture.XXXXXX)"
printf 'visible fixture\n' > "$LS_TEST_DIR/visible.txt"
printf 'hidden fixture\n' > "$LS_TEST_DIR/.hidden.txt"
mkdir "$LS_TEST_DIR/subdir"

if ./aishell ls > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx 'Makefile' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] ls without a directory did not list the current directory." >&2
        exit 1
    fi
    echo "[PASS] ls without a directory defaults to the current directory."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell ls without a directory exited with status $status." >&2
    exit 1
fi

if ./aishell ls "$LS_TEST_DIR" > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx 'visible.txt' tests/aishell_test.log ||
       grep -Fq '.hidden.txt' tests/aishell_test.log ||
       grep -Fqx '.' tests/aishell_test.log ||
       grep -Fqx '..' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] ls default listing did not filter hidden entries correctly." >&2
        exit 1
    fi
    echo "[PASS] ls lists visible entries and filters hidden entries by default."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell ls exited with status $status." >&2
    exit 1
fi

if ./aishell ls -a "$LS_TEST_DIR" > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx '.hidden.txt' tests/aishell_test.log ||
       ! grep -Fqx '.' tests/aishell_test.log ||
       ! grep -Fqx '..' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] ls -a did not include hidden entries and dot directories." >&2
        exit 1
    fi
    echo "[PASS] ls -a lists hidden entries, . and .."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell ls -a exited with status $status." >&2
    exit 1
fi

if ./aishell ls -l "$LS_TEST_DIR" > tests/aishell_test.log 2>&1; then
    if ! grep -Eq '^[-dlcbps][rwxStTs-]{9} ' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] ls -l did not produce long-format metadata." >&2
        exit 1
    fi
    echo "[PASS] ls -l produced long-format metadata."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell ls -l exited with status $status." >&2
    exit 1
fi

if ./aishell ls "$LS_TEST_DIR/no-such-directory" > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] ls unexpectedly opened a nonexistent directory." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]] ||
   ! grep -Fq "$LS_TEST_DIR/no-such-directory:" tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] ls failed with status $status or omitted the directory error." >&2
    exit 1
fi
echo "[PASS] ls reports nonexistent directories and exits with status 1."

if ./aishell ls --unknown "$LS_TEST_DIR" > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] ls unexpectedly accepted an unknown option." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] ls unknown option returned status $status instead of 1." >&2
    exit 1
fi
echo "[PASS] ls rejects unknown options with status 1."

echo "[Stage 4/7] Unknown-command verification"
if ./aishell -z > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] Unknown command -z unexpectedly succeeded." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]] || ! grep -Fqx 'aishell: command not found: -z' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] Unknown command produced status $status or unexpected output." >&2
    exit 1
fi
echo "[PASS] ./aishell -z reports command not found and exits with status 1."

echo "[Stage 5/7] Missing-file-argument error verification"
cat > tests/test_cat_error.c <<'EOF'
#include "cmd_spec.h"

extern cmd_spec_t cmd_cat_spec;

int main(void)
{
    return cmd_cat_spec.run(1, (char *[]){"cat", NULL});
}
EOF

if ! gcc -Wall -Wextra -Iinclude -std=c99 \
    tests/test_cat_error.c \
    src/commands/cmd_cat.c \
    src/registry.c \
    src/argtable3.c \
    -o tests/test_cat_error; then
    echo "[FAIL] Could not compile the cat error-path harness." >&2
    exit 1
fi

if ./tests/test_cat_error > tests/test_cat_error.log 2>&1; then
    status=0
else
    status=$?
fi
cat tests/test_cat_error.log

if [[ "$status" -eq 1 ]]; then
    echo "[PASS] Missing file arguments returned the expected status 1."
else
    echo "[FAIL] Expected status 1 for missing file arguments; got $status." >&2
    exit 1
fi

echo "[Stage 6/7] ls help verification"
if ./aishell ls --help > tests/aishell_test.log 2>&1; then
    if ! grep -Fq 'Usage: ls' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] ls help output did not contain usage." >&2
        exit 1
    fi
    echo "[PASS] ./aishell ls --help displayed command usage."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] ./aishell ls --help exited with status $status." >&2
    exit 1
fi

echo "[Stage 7/7] Additional command verification"
COMMAND_TEST_DIR="$(mktemp -d tests/command-fixture.XXXXXX)"
printf 'line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9\nline 10\nline 11\nline 12' > "$COMMAND_TEST_DIR/lines.txt"

for command_name in stat head tail cp mv rm mkdir rmdir touch pkg; do
    if ! ./aishell "$command_name" -h > tests/aishell_test.log 2>&1 ||
       ! grep -Fq "Usage: $command_name" tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] $command_name -h did not display command usage." >&2
        exit 1
    fi
done
echo "[PASS] All commands display help with -h."

for command_name in stat head tail cp mv rm mkdir rmdir touch; do
    if ./aishell "$command_name" > tests/aishell_test.log 2>&1; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] $command_name accepted missing required operands." >&2
        exit 1
    else
        status=$?
    fi
    if [[ "$status" -ne 1 ]]; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] $command_name missing operands returned status $status." >&2
        exit 1
    fi
done
echo "[PASS] All new commands reject missing required operands."

if ./aishell stat "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1 &&
   grep -Fq 'Size:' tests/aishell_test.log; then
    echo "[PASS] stat reports file metadata."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] stat failed or omitted file size (status $status)." >&2
    exit 1
fi

if ./aishell head -n 2 "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1 &&
   diff -u <(printf 'line 1\nline 2\n') tests/aishell_test.log; then
    echo "[PASS] head -n 2 prints the first two lines."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] head output was incorrect." >&2
    exit 1
fi

if ./aishell head -n invalid "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] head accepted a non-integer line count." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] head invalid line count returned status $status." >&2
    exit 1
fi

if ./aishell head -n 0 "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1 &&
   [[ ! -s tests/aishell_test.log ]]; then
    echo "[PASS] head -n 0 prints no lines."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] head -n 0 did not produce empty output." >&2
    exit 1
fi

if ./aishell head -n 0 "$COMMAND_TEST_DIR/no-such-file" > tests/aishell_test.log 2>&1; then
    echo "[FAIL] head -n 0 ignored a nonexistent file." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] head -n 0 missing input returned status $status." >&2
    exit 1
fi

if ./aishell tail -n 2 "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1 &&
   diff -u <(printf 'line 11\nline 12') tests/aishell_test.log; then
    echo "[PASS] tail -n 2 prints the last two lines."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] tail output was incorrect." >&2
    exit 1
fi

if ./aishell tail -n -1 "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] tail accepted a negative line count." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] tail negative line count returned status $status." >&2
    exit 1
fi

if ./aishell tail -n 0 "$COMMAND_TEST_DIR/lines.txt" > tests/aishell_test.log 2>&1 &&
   [[ ! -s tests/aishell_test.log ]]; then
    echo "[PASS] tail -n 0 prints no lines."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] tail -n 0 did not produce empty output." >&2
    exit 1
fi

if ./aishell tail -n 0 "$COMMAND_TEST_DIR/no-such-file" > tests/aishell_test.log 2>&1; then
    echo "[FAIL] tail -n 0 ignored a nonexistent file." >&2
    exit 1
else
    status=$?
fi
if [[ "$status" -ne 1 ]]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] tail -n 0 missing input returned status $status." >&2
    exit 1
fi

if ./aishell cp "$COMMAND_TEST_DIR/lines.txt" "$COMMAND_TEST_DIR/copy.txt" &&
   cmp -s "$COMMAND_TEST_DIR/lines.txt" "$COMMAND_TEST_DIR/copy.txt"; then
    echo "[PASS] cp copies a regular file."
else
    echo "[FAIL] cp regular-file copy failed." >&2
    exit 1
fi

mkdir "$COMMAND_TEST_DIR/tree"
printf 'tree file\n' > "$COMMAND_TEST_DIR/tree/leaf"
if ./aishell cp -r "$COMMAND_TEST_DIR/tree" "$COMMAND_TEST_DIR/tree-copy" &&
   test -f "$COMMAND_TEST_DIR/tree-copy/leaf" &&
   cmp -s "$COMMAND_TEST_DIR/tree/leaf" "$COMMAND_TEST_DIR/tree-copy/leaf"; then
    echo "[PASS] cp -r copies a directory tree."
else
    echo "[FAIL] cp -r directory copy failed." >&2
    exit 1
fi

printf 'second source\n' > "$COMMAND_TEST_DIR/second.txt"
mkdir "$COMMAND_TEST_DIR/copy-multiple"
if ./aishell cp "$COMMAND_TEST_DIR/lines.txt" "$COMMAND_TEST_DIR/second.txt" \
    "$COMMAND_TEST_DIR/copy-multiple" &&
   cmp -s "$COMMAND_TEST_DIR/lines.txt" "$COMMAND_TEST_DIR/copy-multiple/lines.txt" &&
   cmp -s "$COMMAND_TEST_DIR/second.txt" "$COMMAND_TEST_DIR/copy-multiple/second.txt"; then
    echo "[PASS] cp copies multiple files into a directory."
else
    echo "[FAIL] cp multiple-source copy failed." >&2
    exit 1
fi

if ./aishell mv "$COMMAND_TEST_DIR/copy.txt" "$COMMAND_TEST_DIR/moved.txt" &&
   test ! -e "$COMMAND_TEST_DIR/copy.txt" &&
   cmp -s "$COMMAND_TEST_DIR/lines.txt" "$COMMAND_TEST_DIR/moved.txt"; then
    echo "[PASS] mv renames a file."
else
    echo "[FAIL] mv rename failed." >&2
    exit 1
fi

mkdir "$COMMAND_TEST_DIR/move-source"
mkdir "$COMMAND_TEST_DIR/move-target"
printf 'move directory\n' > "$COMMAND_TEST_DIR/move-source/entry"
if ./aishell mv "$COMMAND_TEST_DIR/move-source" "$COMMAND_TEST_DIR/move-target" &&
   test ! -e "$COMMAND_TEST_DIR/move-source" &&
   test -f "$COMMAND_TEST_DIR/move-target/move-source/entry"; then
    echo "[PASS] mv moves a directory into an existing directory."
else
    echo "[FAIL] mv directory move failed." >&2
    exit 1
fi

printf 'remove me\n' > "$COMMAND_TEST_DIR/remove.txt"
if ./aishell rm "$COMMAND_TEST_DIR/remove.txt" &&
   test ! -e "$COMMAND_TEST_DIR/remove.txt"; then
    echo "[PASS] rm removes a file."
else
    echo "[FAIL] rm file removal failed." >&2
    exit 1
fi

if ./aishell rm -f "$COMMAND_TEST_DIR/does-not-exist"; then
    echo "[PASS] rm -f ignores a nonexistent path."
else
    echo "[FAIL] rm -f did not ignore a nonexistent path." >&2
    exit 1
fi

if ./aishell mkdir -p "$COMMAND_TEST_DIR/new/parent/child" &&
   test -d "$COMMAND_TEST_DIR/new/parent/child"; then
    echo "[PASS] mkdir -p creates parent directories."
else
    echo "[FAIL] mkdir -p failed." >&2
    exit 1
fi

if ./aishell rmdir "$COMMAND_TEST_DIR/new/parent/child" &&
   test ! -e "$COMMAND_TEST_DIR/new/parent/child"; then
    echo "[PASS] rmdir removes an empty directory."
else
    echo "[FAIL] rmdir failed to remove an empty directory." >&2
    exit 1
fi

if ./aishell touch "$COMMAND_TEST_DIR/created.txt" &&
   test -f "$COMMAND_TEST_DIR/created.txt"; then
    echo "[PASS] touch creates a missing file."
else
    echo "[FAIL] touch did not create a missing file." >&2
    exit 1
fi

printf 'preserve this content\n' > "$COMMAND_TEST_DIR/updated.txt"
if ./aishell touch "$COMMAND_TEST_DIR/updated.txt" &&
   grep -Fqx 'preserve this content' "$COMMAND_TEST_DIR/updated.txt"; then
    echo "[PASS] touch updates an existing file without truncating it."
else
    echo "[FAIL] touch changed existing file contents or failed." >&2
    exit 1
fi

if ./aishell rm -r "$COMMAND_TEST_DIR/tree-copy" &&
   test ! -e "$COMMAND_TEST_DIR/tree-copy"; then
    echo "[PASS] rm -r removes a directory tree."
else
    echo "[FAIL] rm -r failed to remove a directory tree." >&2
    exit 1
fi

for command_name in stat head tail cp mv rm mkdir rmdir touch pkg; do
    if ! ./aishell help "$command_name" > tests/aishell_test.log 2>&1 ||
       ! grep -Fq "Usage: $command_name" tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] Help was not available for $command_name." >&2
        exit 1
    fi
done
echo "[PASS] All new command modules expose command-specific help."

echo "[Stage 7b] Package manager (pkg) verification"

# Use an isolated HOME so pkg tests never touch the real ~/.mysh.
PKG_TEST_HOME="$(mktemp -d tests/pkg-home.XXXXXX)"
HOME="$PKG_TEST_HOME" ./aishell pkg list > tests/aishell_test.log 2>&1
if ! grep -Fq "No packages installed." tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg list reported empty install incorrectly." >&2
    exit 1
fi
echo "[PASS] pkg list reports no packages when database is empty."

# Build a mock cli-tools tarball using the project's setup script.
if ! ./setup_test_pkg.sh > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] setup_test_pkg.sh failed to build the mock tarball." >&2
    exit 1
fi
echo "[PASS] setup_test_pkg.sh produced cli-tools-0.1.0.tar.gz."

# Install the mock package and verify binaries land in ~/.mysh/bin.
HOME="$PKG_TEST_HOME" ./aishell pkg install cli-tools-0.1.0.tar.gz \
    > tests/aishell_test.log 2>&1
for bin in cat head stat; do
    if [ ! -x "$PKG_TEST_HOME/.mysh/bin/$bin" ]; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] pkg install did not place an executable $bin in .mysh/bin." >&2
        exit 1
    fi
done
if ! grep -Fq "pkg: cli-tools-0.1.0 installed successfully." tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg install did not report success." >&2
    exit 1
fi
echo "[PASS] pkg install extracts the package and exposes all three executables."

# Run all installed binaries to confirm they are executable and functional.
# Capture output to a file first to avoid SIGPIPE from grep closing the pipe.
for bin in cat head stat; do
    "$PKG_TEST_HOME/.mysh/bin/$bin" Makefile > tests/aishell_test.log 2>&1
    if [ $? -ne 0 ]; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] Installed $bin binary did not execute successfully." >&2
        exit 1
    fi
done
echo "[PASS] All installed package binaries execute correctly."

# pkg list should now report the installed package.
HOME="$PKG_TEST_HOME" ./aishell pkg list > tests/aishell_test.log 2>&1
if ! grep -Fq 'cli-tools-0.1.0' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg list did not show the newly installed package." >&2
    exit 1
fi
echo "[PASS] pkg list shows the installed package."

# pkg info should display metadata for the installed package.
HOME="$PKG_TEST_HOME" ./aishell pkg info cli-tools > tests/aishell_test.log 2>&1
if ! grep -Fq 'Name:    cli-tools' tests/aishell_test.log || \
   ! grep -Fq 'Version: 0.1.0' tests/aishell_test.log || \
   ! grep -Fq 'Desc:    Core command-line utilities bundle for aishell' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg info did not display expected metadata." >&2
    exit 1
fi
# Verify info lists all three binary files.
for f in bin/cat bin/head bin/stat; do
    if ! grep -Fq "$f" tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] pkg info did not list file: $f" >&2
        exit 1
    fi
done
echo "[PASS] pkg info shows name, version, description, and all files."

# pkg info on an unknown package should fail cleanly.
if HOME="$PKG_TEST_HOME" ./aishell pkg info no-such-pkg > tests/aishell_test.log 2>&1; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg info succeeded for an unknown package." >&2
    exit 1
fi
if ! grep -Fqx 'pkg: package not installed: no-such-pkg' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg info did not report the unknown package correctly." >&2
    exit 1
fi
echo "[PASS] pkg info reports unknown package with status 1."

# pkg remove should delete all binaries and the package directory.
HOME="$PKG_TEST_HOME" ./aishell pkg remove cli-tools > tests/aishell_test.log 2>&1
for bin in cat head stat; do
    if [ -e "$PKG_TEST_HOME/.mysh/bin/$bin" ]; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] pkg remove left the binary $bin in .mysh/bin." >&2
        exit 1
    fi
done
if [ -d "$PKG_TEST_HOME/.mysh/pkgs/cli-tools-0.1.0" ]; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg remove left the package directory in .mysh/pkgs." >&2
    exit 1
fi
if ! grep -Fq 'pkg: cli-tools-0.1.0 removed' tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg remove did not report success." >&2
    exit 1
fi
echo "[PASS] pkg remove deletes binaries, package dir, and pkgdb record."

# After removal, pkg list should report no packages again.
HOME="$PKG_TEST_HOME" ./aishell pkg list > tests/aishell_test.log 2>&1
if ! grep -Fq "No packages installed." tests/aishell_test.log; then
    cat tests/aishell_test.log >&2
    echo "[FAIL] pkg list did not report empty after removal." >&2
    exit 1
fi
echo "[PASS] pkg list reports no packages after removal."

rm -rf -- "$PKG_TEST_HOME"

echo "[Stage 7c] REPL interactive loop verification"

# REPL: pipe a registered command (cat) through stdin to confirm the
# REPL dispatches commands in-process. test.txt was created in Stage 2.

# REPL: pipe commands via stdin
if printf 'cat test.txt\nexit\n' | ./aishell > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx 'AiShell cat test sample.' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] REPL cat did not print the expected file contents." >&2
        exit 1
    fi
    echo "[PASS] REPL dispatches commands in-process."
else
    status=$?
    cat tests/aishell_test.log >&2
    echo "[FAIL] REPL exited with status $status." >&2
    exit 1
fi

# REPL: help inside the loop
if printf 'help cat\nexit\n' | ./aishell > tests/aishell_test.log 2>&1; then
    if ! grep -Fqx 'cat - print file contents' tests/aishell_test.log; then
        cat tests/aishell_test.log >&2
        echo "[FAIL] REPL help cat did not show command help." >&2
        exit 1
    fi
    echo "[PASS] REPL help <cmd> displays command documentation."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] REPL help failed." >&2
    exit 1
fi

echo "[Stage 7d] BusyBox symlink multicall verification"

# Create symlinks named after commands (in a scratch dir) pointing to aishell.
# When invoked via a symlink whose basename matches a registered command,
# AiShell detects this and dispatches that command directly.
SYMLINK_DIR="$(mktemp -d tests/symlink-fixture.XXXXXX)"

# ls symlink
ln -s "$PWD/aishell" "$SYMLINK_DIR/ls"
if "$SYMLINK_DIR/ls" > tests/aishell_test.log 2>&1 &&
   grep -Fq 'Makefile' tests/aishell_test.log; then
    echo "[PASS] Symlink multicall (ls) dispatches correctly."
else
    cat tests/aishell_test.log >&2
    echo "[FAIL] Symlink multicall did not dispatch ls correctly." >&2
    exit 1
fi

# Cleanup
rm -rf -- "$SYMLINK_DIR"

echo "[PASS] All verification stages completed."
