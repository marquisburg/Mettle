# Shared libraries

ELF `.so` and Windows DLL. The internal ELF linker binds a program to a `.so`,
emits a `.so` of its own, and publishes a program's symbols so a library can
call back into it. On Windows `--build --shared` emits a DLL; the import side
(DLL import probe) is in [C interop](c-interop.md). `-l`, `-L`, `--rpath`,
`--export-dynamic` and `--dynamic-linker` stay ELF-only; a PE build takes its
libraries through `--link-arg`.

## Linking against one

```bash
mettle --build app.mettle -o app -L/opt/acme/lib -lacme --rpath /opt/acme/lib
```

| Option | Meaning |
|--------|---------|
| `-lname`, `--library name` | Bind `libname.so`. A value containing `/` is used as a path. `-l:libz.so.1` names a file exactly. |
| `-Ldir`, `--library-path dir` | Search here before the platform directories. |
| `--rpath dir` | Record `dir` in `DT_RUNPATH`. Repeatable; the values are joined with `:`. |
| `--dynamic-linker path` | What `PT_INTERP` names. Defaults to `/lib64/ld-linux-x86-64.so.2`. |
| `--export-dynamic`, `-rdynamic` | Publish the program's own globals in `.dynsym`. |

`-l` and `-L` must be written attached to their value, because a bare `-l` is
already `--line-mapping`.

Declare what you use the way any other foreign function is declared:

```mettle
extern fn crc32(crc: int64, buffer: cstring, length: int32) -> int64 = "crc32";
extern var shared_counter: int32 = "shared_counter";
```

A library search follows `-L` in order, then `/usr/local/lib/x86_64-linux-gnu`,
`/usr/local/lib64`, `/usr/local/lib`, `/usr/lib/x86_64-linux-gnu`, `/usr/lib64`,
`/usr/lib`, `/lib/x86_64-linux-gnu`, `/lib64`, `/lib`. A hit that turns out to
be an `ld` script rather than an ELF file is followed to the shared object it
names, which is how `-lc` and `-lm` resolve on Debian and Ubuntu.

Only libraries something actually needs reach `DT_NEEDED`. Naming one no symbol
comes from costs nothing.

### A worked example

raylib is the case this was built for. Declare what you call, pass its structs
by value, and link the release directory:

```mettle
struct Color { r: uint8; g: uint8; b: uint8; a: uint8; }
struct Vector2 { x: float32; y: float32; }

extern fn InitWindow(width: int32, height: int32, title: cstring) = "InitWindow";
extern fn BeginDrawing() = "BeginDrawing";
extern fn ClearBackground(color: Color) = "ClearBackground";
extern fn DrawCircleV(centre: Vector2, radius: float32, color: Color) = "DrawCircleV";
extern fn EndDrawing() = "EndDrawing";
extern fn CloseWindow() = "CloseWindow";
```

```bash
mettle --build game.mettle -o game -L raylib/lib -lraylib --rpath raylib/lib
```

That opens a GLFW window, takes an OpenGL context and draws. A struct literal
carries no type name before the brace: `var white: Color = { r: 245, g: 245,
b: 245, a: 255 };`. Where a distribution ships no `-dev` symlink, name the file
outright: `-l:libGL.so.1`, `-l:libX11.so.6`.

## What the linker emits

A program that binds a library gains `.interp`, `.dynsym`, `.dynstr`, `.hash`,
`.rela.dyn`, `.rela.plt`, `.plt`, `.got.plt` and `.dynamic`, plus `PT_INTERP`,
`PT_DYNAMIC` and `PT_GNU_STACK`.

- **A function** binds through a PLT stub. The stub is one `jmp` through its
  GOT slot; `DF_BIND_NOW` and `DF_1_NOW` are set, so the loader resolves every
  slot before the program runs and there is no lazy-binding trampoline.
- **A data symbol** binds through `R_X86_64_COPY`. Storage of the symbol's
  recorded size moves into the program's `.bss`, the program exports it, and
  the library's own references bind to that one copy. Writes are visible on
  both sides.
- **A versioned symbol** carries `.gnu.version` and `.gnu.version_r`, so glibc
  binds the same version `ld` would have picked rather than whichever default
  happens to be first.

The image stays `ET_EXEC` at `0x400000`. Nothing about it is position
independent; the loader is there to bind names, not to move the program.

## Emitting one

```bash
mettle --build --shared lib.mettle -o libacme.so --soname libacme.so
```

The result is `ET_DYN` with a `DT_SONAME`, no entry point and no `PT_INTERP`.
There is no `_start`, so no `main` is needed.
`export fn` decides what appears in `.dynsym`; the bundled
runtime rides along so the library is self-contained but is never published, so
loading it does not interpose someone else's `malloc`. Absolute addresses in
the image become `R_X86_64_RELATIVE`, and because the code generator is not
position independent the library is marked `DT_TEXTREL`: the loader makes the
text writable while it relocates.

`DT_SYMBOLIC` is set. A call inside the library to a name the library itself
defines binds to that definition and cannot be interposed from outside.

A symbol no input defines stays undefined in `.dynsym` and is bound by whoever
loads the library. Building the program with `--export-dynamic` is what makes
its definitions available for that.

```bash
mettle --build --shared plugin.mettle -o libplugin.so --soname libplugin.so
mettle --build host.mettle -o host -L. -lplugin --rpath "$PWD" --export-dynamic
```

## Emitting a Windows DLL

```powershell
mettle --build --shared lib.mettle -o acme.dll --soname acme.dll
```

The result sets `IMAGE_FILE_DLL`, has no entry point (`AddressOfEntryPoint` is
zero, so no `main` or `DllMain` is needed), and publishes the user globals
through the export directory (`.edata` in `.rdata`): `export fn` functions and
`export var` variables, sorted by name. Compiler-owned `mettle_*` tables and
the bundled runtime stay private, so loading the DLL does not interpose
someone else's `malloc`. `--soname` sets the export name; without it the output
file name is used. No import library (`.lib`) is emitted; load the DLL with
`LoadLibrary`/`GetProcAddress`.

A DLL may import functions (a Win32 API declared `extern fn` binds through the
usual import table). Like the ELF `.so`, the image is not position independent:
no `.reloc` is emitted yet, so the loader must place it at its preferred base.

## What does not work

- **A shared object cannot reference imported data.** The code generator emits
  absolute addresses, and a library moves. Importing a function is fine;
  importing a variable is refused, by name, at link time. A program has copy
  relocations for this and is unaffected.
- **A shared object holds no thread-local storage.** A thread-local is reached
  at a fixed offset from the thread pointer, and only a program knows that
  offset. `--shared` therefore links `freestanding_shared.o` and
  `safety_shared.o`, builds of the runtime that keep `errno` per process rather
  than per thread. A thread-local reaching the link from a foreign object is
  refused by name.
- **The runtimes stay separate.** A library built by Mettle carries its own
  allocator. Memory it returns must be freed by it, not by the program's
  `free`, and the same holds in reverse.
- **Only the main thread may call a foreign library.** Threads Mettle starts
  install their own thread pointer, which a C library's own thread-local state
  will not recognize.
- **x86-64 only.** The ELF image writer emits `EM_X86_64`.

## The ownership audit

An owned build refuses `PT_INTERP` and `PT_DYNAMIC`, which is what makes a
plain Mettle program provably free of a foreign runtime. Asking for `-l`,
`--shared` or `--export-dynamic` states the intent, so the audit allows the
dynamic segments for that link and checks the rest as usual. A build that names
no library is audited exactly as before.

There is no fallback: a link that binds libraries never quietly reruns through
`ld` or `gcc`, because that would produce an image with a runtime this compiler
does not own. A failure is reported instead.

## See also

- [C interop](c-interop.md)
- [Linker and build pipelines](linker-build-pipelines.md)
- [Runtime model](runtime-model.md)
