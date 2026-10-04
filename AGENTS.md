# AGENTS.md — rules for AI coding agents working in this repo

## Git hygiene (STRICT)

1. **NEVER use `git add -A`, `git add .`, `git add --all`, or any wildcard/bulk-add.**
   Stage files one at a time, by explicit path:
   `git add efs.h efs_mph.h` — listing exactly the source files you changed or added.
2. **Only commit files you actually changed or added as source.**
   Never commit build artifacts: no `*.o`, no executables/ELF binaries, no generated
   images/blobs. If `git status` shows an untracked artifact after a build, delete it
   (`make clean`) or leave it untracked — never stage it.
3. **Never modify `.gitignore` to hide artifacts** unless the user explicitly asks for
   a `.gitignore` change as part of the task. Do not "fix" dirty-status problems by
   silencing them.
4. Before committing, run `git status --short` and verify the staged set contains
   ONLY intended source changes; before pushing, verify with `git show --stat HEAD`.
5. Do not amend or rewrite commits you did not create in this session unless asked.

## Build & verification

- Default algorithm is JMPH: `make check`.
- BBHash selection: `make MPH=BBHASH check`.
- Run both checks when touching shared layout code (`efs.h`, `efs_mph.h`, `bbhash.h`,
  `jmph.h`, `efsbuilder.c`).
- After verification runs, execute `make clean` so no artifacts remain in the tree.

## Code conventions

- The EFS header is 20 bytes; `struct efs_dir` has a `_Static_assert` under `EFS_IMPL`.
  Any layout change must update that assert deliberately, never silently.
- Algorithm-specific parameters live only in `union mph_params` inside `struct efs_dir`;
  the unified size function takes the union and each backend reads its own angle
  (`p.bb_sz` for BBHash, `p.jmph.w` for jmph). Do not widen shared header fields.
- `mph_out`/`jmph_out` structs have their own `.w`/`.len` fields — these are builder
  outputs, NOT header fields; never rename them mechanically when touching `efs_dir`.
