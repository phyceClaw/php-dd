# dd — Laravel's `dd()`/`dump()` as a native PHP extension

A compiled C extension (Zend engine) that provides `dd()` and `dump()` as
built-in language functions instead of userland helpers. Output mirrors
Symfony VarDumper's default theme — the same dumper Laravel uses.

## Functions

| Function          | Behaviour                                                         |
|-------------------|------------------------------------------------------------------|
| `dd(...$vars)`    | Dump every argument, then halt the process (`exit(1)`).          |
| `dump(...$vars)`  | Dump every argument, then return them (1 arg ⇒ that arg; many ⇒ array of args). |

## Output auto-detection

| Context                       | Format                                   |
|-------------------------------|------------------------------------------|
| Web SAPI                      | styled HTML `<pre class=sf-dump>` block  |
| CLI on a TTY                  | ANSI 256-colour                          |
| CLI piped, or `NO_COLOR` set  | plain text                               |

The colour palette, `array:N [...]` / `Class {#id ...}` layout, and the
`+` public / `#` protected / `-` private visibility markers all match
Symfony VarDumper.

## Build

```sh
phpize
./configure --enable-dd
make
```

Produces `modules/dd.so`.

## Use it ad-hoc

```sh
php -d extension="$PWD/modules/dd.so" your-script.php
```

## Enable permanently

```sh
sudo cp modules/dd.so "$(php-config --extension-dir)/"
# CLI:
echo 'extension=dd.so' | sudo tee /etc/php/8.4/cli/conf.d/20-dd.ini
# add an equivalent file under fpm/conf.d for web requests.
```

Because `dd`/`dump` become real internal functions, Laravel's and Symfony's
userland definitions (both guarded by `if (! function_exists(...))`) silently
defer to these — no "cannot redeclare" fatal.

## Scope / limitations

- Handles null, bool, int, float, string, array, object (with visibility &
  uninitialized typed properties), resource; circular references are detected
  and shown as `*RECURSION*`.
- Floats use shortest round-tripping form (`serialize_precision = -1`).
- HTML output is styled and escaped but **static** — the interactive
  collapse/expand JavaScript widget from Symfony VarDumper is not reproduced.
- Closures, and Symfony's per-class "casters" (Carbon, collections, etc.),
  are dumped as plain objects rather than with bespoke formatting.
