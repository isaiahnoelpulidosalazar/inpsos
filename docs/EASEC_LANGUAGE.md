# Easec language reference (v2)

Easec is a small, newline-terminated, lexically scoped language that compiles to bytecode.
v2 keeps the English-flavoured keywords (`say`, `var`, `job`, `out`, `repeat`, `note`) but replaces the
context-sensitive parts of v1 with a regular grammar.

## At a glance
```
note this is a comment
note { block comments nest { like this } }

var name = "inpsos"            note declaration (always with '=')
var count: number = 0          note optional type: text number decimal boolean list dict
count += 1                     note assignment is a STATEMENT; also -= *= /=

say "Hello, " + name           note `say` alone prints an empty line

job add(a, b) {                note jobs: explicit parameter list, explicit calls
    out a + b                  note `out` returns (no value -> null)
}
say add(15, 25)

if count > 3 and not done {
    say "big"
} else if count == 3 {         note `else` must be on the same line as the closing brace
    say "three"
} else {
    say "small"
}

while count < 10 { count += 1 }
repeat 5 { say "hi" }
repeat forever {
    if time.uptime() > 5000 { stop }     note stop = break, skip = continue
}

var xs = [1, 2, 3]             note lists
xs[0] = 10
push(xs, 4)
var cfg = {host: "localhost", "port": 8080}   note dictionaries (unordered)
cfg.host = "127.0.0.1"
say cfg["port"]

import "datetime.modec" as dt  note alias defaults to the file name without extension
say dt.today()
```

## Values
`null`, `true`/`false`, whole numbers (64-bit), decimals, text, lists, dictionaries, jobs, modules.
* Falsy: `null`, `false`, `0`, `0.0`. Everything else (including `""`) is truthy.
* `int / int` is integer division; use a decimal operand (`7 / 2.0`) for fractions. `%` needs whole numbers.
* `+` concatenates when either side is text: `"n=" + 5` -> `n=5`.
* `==` compares numbers by value (`1 == 1.0`), text by content, lists/dicts by identity.
* Text supports escapes `\n \t \r \" \\ \0` and may span lines. `"abc"[1]` is `"b"`.
* Decimals print with up to 6 digits after the point, trailing zeros trimmed.

## Operators (low to high precedence)
`or` < `and` < `== !=` < `< <= > >=` < `+ -` < `* / %` < unary `-` < call `()` / index `[]` / member `.`
`not` binds looser than comparison: `not a == b` means `not (a == b)`. `and`/`or` short-circuit and return an operand.

## Statements
| Statement | Notes |
|---|---|
| `var x = e`, `var x: type = e`, `var x` | `var x` declares `null`. A type annotation is checked once, at declaration (`decimal` accepts a number and converts it). |
| `x = e`, `x += e`, `a[i] = e`, `d.k = e`, `d.k += e` | Only valid as a statement. `if x = 1 {` is a compile error, not a silent assignment. Assigning to an undeclared name is a runtime error. |
| `say e` | |
| `job f(a, b) { ... }` | Closures capture their defining environment. Recursion works. Jobs have a flat scope (blocks do not create scopes). |
| `out e` | Return. At top level it ends the script. |
| `if` / `else if` / `else` | Braces are mandatory. |
| `while c {}`, `repeat n {}`, `repeat forever {}` | `stop` and `skip` work in all three. `repeat n` evaluates `n` once. |
| `import "path" [as alias]` | Modules run once and are cached; cycles see the partially initialised module. |

Statements end at a newline (or before `}`). Newlines are allowed after operators and commas and inside `()`, `[]`, `{}` literals.
`note` is a reserved word.

## Built-in jobs
| Job | Result |
|---|---|
| `input()` / `input(prompt)` | next line of input as text, or `null` at end of input |
| `len(x)` | length of text, list or dictionary |
| `text(x)`, `number(x)`, `decimal(x)` | conversions; unparsable text gives `null` |
| `type(x)` | `"null" "boolean" "number" "decimal" "text" "list" "dict" "job" "module"` |
| `push(list, v)`, `pop(list)` | grow / shrink a list (`push` returns the new length) |
| `keys(dict)` | list of keys (unordered) |

## Modules `time` and `file` (always available)
| Call | Result |
|---|---|
| `time.now()` | wall clock, Unix epoch milliseconds (UTC, from the RTC) |
| `time.uptime()` | milliseconds since boot (monotonic) |
| `time.sleep(ms)` | |
| `file.create(name, text)` / `file.append(name, text)` | `true` / `false`; create replaces, append creates if missing |
| `file.read(name)` | text, or `null` if missing |
| `file.exists(name)`, `file.delete(name)` | `true` / `false` |
| `file.list()` | list of file names |

File names are 1-31 printable characters without spaces. The disk holds at most 10 files.

## Errors
Compile errors are reported as `file:line: Error at 'tok': message`. Runtime errors print a message and a
call trace (`[line N] in job`). Recursion is limited to 128 frames, and each program has a 1 MiB budget
(the garbage collector runs well before that).

---

## Migrating from v1

| v1 | v2 |
|---|---|
| `[ ... ]` blocks | `{ ... }` blocks (`[ ]` is now list syntax) |
| `var text msg "hi"` | `var msg: text = "hi"` (type optional, now checked) |
| `var x get` | `var x = input()` |
| `x = x + 1` | unchanged; `x += 1` also works |
| `job add a, b [ out a + b ]` | `job add(a, b) { out a + b }` |
| `var t add 15, 25` (implicit call) | `var t = add(15, 25)` |
| `factorial n - 1` | `factorial(n - 1)` (v1 parsed this ambiguously) |
| zero-arg job auto-called when named | `f()` is required; naming `f` gives the job itself |
| `array text xs 1, 2, 3` | `var xs = [1, 2, 3]` |
| `array get xs 1` / `array set xs 1 v` | `xs[1]` / `xs[1] = v` |
| `dictionary text d a: 1, b: 2` | `var d = {a: 1, b: 2}` |
| `dictionary get d key` / `dictionary set d key v` | `d.key` or `d["key"]` / `d.key = v` |
| `repeat n [ ... ]`, `repeat forever [ ... ]` | same keywords with `{ }`; new: `while`, `stop`, `skip` |
| `out` inside `repeat forever` to leave a loop | `stop` |
| `time get` / `time sleep 100` | `time.now()` / `time.sleep(100)` |
| `file create f c` / `file update f c` / `file delete f` | `file.create(f, c)` / `file.append(f, c)` / `file.delete(f)` |
| globals `files`, `file_count` | `file.list()` |
| `note text` / `note [ ... ]` | `note text` / `note { ... }` |
| `"line\nbreak"` (never worked; README only) | works |
| `if a == 1 or b` (no `and`/`or`/`not`) | `and`, `or`, `not` exist |
| README `//` comments | never existed; the comment keyword is `note` |
| `import "math.easec"` in the README | modules are `.modec` files: `import "math.modec" as m` |
