# pycoolctl class

Query classes from a pycools index.

## Usage

```
pycoolctl class [NAME] [--file PATH] [--module PATH] [--info LETTERS]
               [--compact|--full|--verbose|--json] [--db PATH]
```

- `NAME`: class name (optional; lists all if omitted).
- `--file` / `--module`: restrict to one source file.
- `--info LETTERS`: select fields (default verbose only when used).
- `--compact`: one compact line per class (default).
- `--full` / `--verbose`: verbose `key=value | ...` output.
- `--json`: JSON array.
- `--db PATH`: index database (or first running instance).

## Info letters

- `A` = name
- `B` = file
- `C` = line
- `D` = bases
- `E` = methods
- `F` = fields (n/a)
- `G` = docstring
- `H` = types (n/a)
- `I` = refs (n/a)
- `J` = decorators
- `K` = module

## Output

- Compact: `Name path:line [bases=...] [methods=...] [docstring=...] [decorators=...]`
- Empty result: no output.
- Stable, one line per class.