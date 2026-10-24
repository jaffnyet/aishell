#!/usr/bin/env bash
#
# setup_test_pkg.sh
#
# Generate a mock "cli-tools" package under fixtures/cli-tools/ and package it
# into cli-tools-0.1.0.tar.gz in the project root for package-management
# testing. Cleans up any previous artifacts before building.
#
# The cli-tools package bundles three filesystem utilities: cat, head, and stat.

set -euo pipefail

# Resolve the project root (directory containing this script).
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
PKG_NAME="cli-tools"
PKG_VERSION="0.1.0"
PKG_TARBALL="$PKG_NAME-$PKG_VERSION.tar.gz"
PKG_DIR="fixtures/$PKG_NAME"

# Clean up any previous artifacts so we start fresh.
echo "[setup_test_pkg] Cleaning up previous artifacts..."
rm -rf -- "$PKG_DIR" "$PKG_TARBALL" "${PROJECT_ROOT}/${PKG_DIR}" "${PROJECT_ROOT}/${PKG_TARBALL}"

# Recreate the package directory structure.
mkdir -p "$PKG_DIR/bin"
mkdir -p "$PKG_DIR/docs"

# --- pkg.json : package manifest adhering to the course schema ---
cat > "$PKG_DIR/pkg.json" <<'EOF'
{
    "name": "cli-tools",
    "version": "0.1.0",
    "description": "Core command-line utilities bundle for aishell",
    "commands": ["cat", "head", "stat"],
    "files": ["bin/cat", "bin/head", "bin/stat"],
    "docs": ["docs/cat.md", "docs/head.md", "docs/stat.md"],
    "dependencies": []
}
EOF

# --- bin/cat : executable shell script (mock cat utility) ---
# Reads and displays the contents of a target file passed as an argument.
cat > "$PKG_DIR/bin/cat" <<'EOF'
#!/usr/bin/env bash
# cat - concatenate and display file contents (core CLI utility for aishell)
# Reads and displays the contents of a target file passed as an argument.

if [[ $# -eq 0 ]]; then
    echo "cat: missing file operand" >&2
    exit 1
fi

for f in "$@"; do
    if [[ -e "$f" ]]; then
        while IFS= read -r line || [[ -n "$line" ]]; do
            printf '%s\n' "$line"
        done < "$f"
    else
        echo "cat: cannot read '$f': No such file or directory" >&2
        exit 1
    fi
done
EOF
chmod +x "$PKG_DIR/bin/cat"

# --- bin/head : executable shell script (mock head utility) ---
# Outputs the first 5 lines of a target file.
cat > "$PKG_DIR/bin/head" <<'EOF'
#!/usr/bin/env bash
# head - output the first lines of a file (core CLI utility for aishell)
# Outputs the first 5 lines of a target file.

if [[ $# -eq 0 ]]; then
    echo "head: missing file operand" >&2
    exit 1
fi

target="$1"
if [[ -e "$target" ]]; then
    n=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        printf '%s\n' "$line"
        n=$((n + 1))
        if [[ $n -ge 5 ]]; then
            break
        fi
    done < "$target"
else
    echo "head: cannot read '$target': No such file or directory" >&2
    exit 1
fi
EOF
chmod +x "$PKG_DIR/bin/head"

# --- bin/stat : executable shell script (mock stat utility) ---
# Outputs simulated file metadata (size, file type, permissions).
cat > "$PKG_DIR/bin/stat" <<'EOF'
#!/usr/bin/env bash
# stat - file status inspector (core CLI utility for aishell)
# Outputs simulated file metadata: size, file type, permissions.

if [[ $# -eq 0 ]]; then
    echo "  File: ."
    echo "  Size: 4096       File Type: directory"
    echo "  Permissions: drwxr-xr-x"
    echo "  Owner: $(id -un)    Group: $(id -gn)"
    echo "  Accessed: 2026-01-01 00:00:00.000000000 -0700"
    echo "  Modified: 2026-01-01 00:00:00.000000000 -0700"
elif [[ $# -gt 0 ]]; then
    target="$1"
    if [[ -e "$target" ]]; then
        size=$(stat -c '%s' "$target" 2>/dev/null || echo 0)
        if [[ -d "$target" ]]; then
            ftype="directory"
            perms="drwxr-xr-x"
        elif [[ -f "$target" ]]; then
            ftype="regular file"
            perms="-rw-r--r--"
        else
            ftype="other"
            perms="??????????"
        fi
        echo "  File: $target"
        echo "  Size: $size       File Type: $ftype"
        echo "  Permissions: $perms"
        echo "  Owner: $(id -un)    Group: $(id -gn)"
        echo "  Accessed: 2026-01-01 00:00:00.000000000 -0700"
        echo "  Modified: $(stat -c '%y' "$target" 2>/dev/null || echo "2026-01-01 00:00:00.000000000 -0700")"
    else
        echo "stat: cannot stat '$target': No such file or directory" >&2
        exit 1
    fi
fi
EOF
chmod +x "$PKG_DIR/bin/stat"

# --- docs/cat.md : documentation for the cat command ---
cat > "$PKG_DIR/docs/cat.md" <<'EOF'
# cat

Concatenate and display file contents.

## Usage

```
cat [file ...]
```

## Description

Reads the specified file(s) and writes their contents to standard output.

## Example

```
$ cat Makefile
... (file contents) ...
```
EOF

# --- docs/head.md : documentation for the head command ---
cat > "$PKG_DIR/docs/head.md" <<'EOF'
# head

Output the first lines of a file.

## Usage

```
head [file]
```

## Description

Prints the first 5 lines of the specified file to standard output.

## Example

```
$ head Makefile
... (first 5 lines) ...
```
EOF

# --- docs/stat.md : documentation for the stat command ---
cat > "$PKG_DIR/docs/stat.md" <<'EOF'
# stat

File status inspector — display file metadata.

## Usage

```
stat [file]
```

## Description

Outputs simulated file metadata including size, file type, permissions,
ownership, and timestamps for the specified path. When no file is given,
prints status for the current directory.

## Example

```
$ stat Makefile
  File: Makefile
  Size: 1387       File Type: regular file
  Permissions: -rw-r--r--
```
EOF

echo "[setup_test_pkg] Created package structure under $PKG_DIR/"

# Validate pkg.json is well-formed JSON before archiving.
if ! python3 -m json.tool "$PKG_DIR/pkg.json" >/dev/null 2>&1; then
    echo "[setup_test_pkg] [FAIL] pkg.json is not valid JSON." >&2
    exit 1
fi
echo "[setup_test_pkg] pkg.json validates as JSON."

# --- Package the contents into a gzip tarball in the project root ---
# Archive the fixtures/cli-tools directory with a clean "cli-tools/"
# top-level prefix by changing into the project root first.
echo "[setup_test_pkg] Creating archive $PKG_TARBALL in $PROJECT_ROOT..."
( cd "$PROJECT_ROOT/fixtures" && tar -czf "../$PKG_TARBALL" "$PKG_NAME" )

echo "[setup_test_pkg] Archive created: $PKG_TARBALL"

# --- Print the archive contents to confirm structure ---
echo "[setup_test_pkg] Archive contents:"
tar -tzf "$PROJECT_ROOT/$PKG_TARBALL"
