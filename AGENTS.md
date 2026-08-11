# Coroutine

A minimal, embeddable **C fiber / asymmetric-coroutine** library (see `README.md`, in Traditional Chinese). Pure C99/C11 plus per-architecture assembly for context switching. No package manager, no external dependencies, no runtime services (no servers, databases, or network components).

## Cursor Cloud specific instructions

### Scope / what "running the app" means
This is a library, not a service. There is nothing long-running to keep alive. "Running" it means compiling the library for the host platform and linking it into a small program that exercises the API (`co_create` -> `co_resume` -> `co_yield_now` -> `co_destroy`).

### Toolchain
`gcc`, `clang`, and GNU `make` are preinstalled in the base image; there are no dependencies to install. The active platform here is Linux x86_64, which uses the System V backend under `platform/Linux_x86/`.

### Building on this VM (Linux x86_64)
Compile the API + the System V backend (C file + `.S` assembly) together, e.g.:
```
gcc -std=c11 -Wall -Wextra -O2 -I. -Iplatform/Linux_x86 \
    coroutine.c platform/Linux_x86/SystemV.c platform/Linux_x86/sysV.S \
    your_program.c -o your_program -lpthread
```
`co_context.h` auto-selects the backend from predefined macros (`__x86_64__`, `_WIN32`, `__aarch64__`, `__APPLE__`), so include order matters: include `coroutine.h` from your program.

### Lint / test / build gotchas
- **`master` has no Makefiles or test suite.** The `README.md` references `Makefile*`, `run_*.sh`, `Plan.md`, and `tests/` that do NOT exist on `master`. Those build/test files live only on the `origin/test/edge-cases-threads` branch, and the library sources there have **diverged** from `master` (different `co_resume` / `co_yield_now` signatures). Do not run that branch's tests against `master` sources — they target a different API. On `master`, verify changes by compiling a small driver program instead.
- **Lint proxy:** there is no linter config; compile with `-Wall -Wextra` and treat warnings as the lint signal. A clean `master` build is warning-free (a `void*`-subtraction warning appears only under `-Wpedantic`).
- **AddressSanitizer caveat:** the library installs its own `SIGSEGV` handler (`co_segv_handler`) for guard-page overflow detection. Under `-fsanitize=address` this conflicts with ASan's fault handling and the first fiber switch aborts inside `__asan_handle_no_return` ("SIGSEGV outside any coroutine guard page"); `ASAN_OPTIONS=handle_segv=0` does not fix it. Prefer a plain (non-ASan) build for smoke-testing on `master`.

### Current API (master)
`co_resume(coroutine*, void *arg, void **out)` and `co_yield_now(void *arg, void **out)` take value-transfer params (the `README.md` minimal example still shows the older no-arg signatures and will not compile as-is on `master`). The coroutine entry argument comes from the first `co_resume`'s `arg`, not from `co_create`'s `argument` (which is only stored as userdata).
