# codecat

Concatenate source files from a directory tree into a single annotated text file.

## What’s new
- **Positional root**: `codecat src`
- **Timestamped output by default**: `codecat_YYYYMMDDHHMMSS.txt` (overridden by `-o`)

## Build & Install (Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential
make
sudo make install
```

Use Clang (optional):

```bash
make clean
CC=clang make
sudo make install
```

## Quick Examples

Scan `src/` and write to a timestamped file:

```bash
codecat src
# -> ./codecat_YYYYMMDDHHMMSS.txt
```

Scan current repo and write timestamped:

```bash
codecat .
```

Explicit output filename:

```bash
codecat -r src -i mydump.txt
```

Rust workspace, default excludes already applied:

```bash
codecat . --exts .rs,.toml,.md
```

Include dotfiles:

```bash
codecat . --include-hidden
```

Follow symlinks:

```bash
codecat . --folow-links
```

## Options

```scss
-r, --root <path>           Root directory (or use positional [root])
-o, --out  <file>           Output file (overrides timestamped default)
    --exts <list>           Comma-separated extensions (default has many)
    --exclude-dirs <list>   Comma-separated dir names to skip
    --follow-links          Follow symlinks (default: off)
    --include-hidden        Include dotfiles & dotdirs (default: off)
-h, --help                  Show help
```

## Output format

```diff
==================== BEGIN FILE: /abs/path/to/file.ext ====================
<file contents...>
===================== END FILE =====================
```

Prints a summary with final file size on completion

## Uninstall

```bash
sudo make uninstall
```

## License
MIT


