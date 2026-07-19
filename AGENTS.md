# MiniKV repository guidance

## Scope

MiniKV is an educational key-value database written in C17. The M0 milestone is
only a buildable and testable project skeleton.

Do not add networking, epoll, RESP, hash-table, AOF, configuration, threading,
or third-party runtime dependencies unless a later task explicitly requires
them.

## Layout

- Put executable entry points in `apps/`.
- Put reusable implementation in `src/`.
- Put public headers below `include/minikv/`.
- Put lightweight C tests in `tests/`.
- Keep generated files below `build/`.

Do not add unused future-module directories or placeholder files.

## Build and test

Use separate build trees for Windows and WSL because their generated files are
not compatible.

Windows:

```powershell
cmake -S . -B build/windows
cmake --build build/windows --config Debug
ctest --test-dir build/windows -C Debug --output-on-failure
```

WSL/Linux:

```sh
cmake -S . -B build/wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build/wsl
ctest --test-dir build/wsl --output-on-failure
```

Do not commit generated build files.

## Transparency and communication

Before each meaningful step, Codex must explain in Chinese:

- What it will do.
- Why the step is needed.
- Which files it will modify.
- Which commands it will run.
- The expected result.
- The relevant risks.

After each step, Codex must explain:

- What it actually completed.
- Whether it succeeded.
- Important output or errors.
- Which files it modified.
- What the next step is.

Codex may combine repetitive read-only checks into one step, but must not
silently batch unrelated operations.

## Coding rules

- Use C17.
- Keep code portable across Linux, GCC, and Clang.
- Do not add unapproved third-party runtime dependencies.
- Do not create unused future modules in advance.
- Do not call `exit()` from library code; return an error to the caller.
- When a length is already known, do not repeatedly call `strlen()`.
- Check for overflow before arithmetic involving `size_t`.
- Never assign the result of `realloc()` directly to the original pointer.
- Document memory ownership and lifetime for every new public interface.
- Do not perform refactoring or formatting unrelated to the current task.

## Testing rules

- Do not claim that a test passed unless it actually ran successfully.
- Record a test that did not start, a test blocked by system policy, and a test
  assertion failure as three distinct outcomes.
- Add a regression test for every bug fix.
- When changing protocol parsing, test fragmented input and invalid input.
- When changing network code, cover `EAGAIN`, partial reads, and partial writes.
- When changing memory-related code, run each applicable Sanitizer.
- If a required tool is missing, report it; do not install it.

## Review priorities

Review changes in this order:

1. Memory safety and ownership.
2. Array and parser boundaries.
3. Integer and `size_t` overflow.
4. File descriptor and resource lifetimes.
5. Partial I/O and error paths.
6. Persistence consistency.
7. Test coverage.
8. Changes outside the task scope.

## Stop and request approval

Codex must stop and request approval before:

- Modifying a file outside the task scope.
- Creating an unplanned file.
- Deleting a file.
- Changing a public interface.
- Adding a dependency.
- Installing software.
- Changing system configuration or security policy.
- Running a destructive command.
- Running `git commit`, `git push`, `git reset --hard`, `git clean`, or a force
  push.
- Choosing among multiple reasonable fixes for a build failure.
- Lowering the testing standard.
- Modifying a file outside the repository.

## Git rules

- Codex must not commit or push without explicit approval.
- Do not use `git add .`; stage an explicit file list instead.
- Do not use `git reset --hard`, `git clean -fdx`, or force push.
- Keep all build artifacts below `build/` and do not commit them.
