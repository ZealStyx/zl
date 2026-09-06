# Packages, Imports, and Dependencies

- [Imports](#imports)
- [The stdlib root](#the-stdlib-root)
- [External dependencies (`zlpkg`)](#external-dependencies-zlpkg)

## Imports

Java-style dotted imports: `import io.github.test.Helpers` resolves to
`<sourceRoot>/io/github/test/Helpers.zl`, where `sourceRoot` is the nearest ancestor
directory literally named `src` — or, for flat single-file programs with no imports, the
entry file's own directory.

Transitive imports, diamond imports, and circular-import detection are all handled.

## The stdlib root

On top of a project's own imports, the interpreter searches a **stdlib root**:
`$ZL_STDLIB_ROOT` if set, otherwise `<the zl_language executable's directory>/stdlib`.

- **`zl.lang`** is auto-imported everywhere, no `import` needed.
- **Everything else** requires an explicit `import`, and the compiler rejects a call to
  a package that was not imported in that file — `import zl.util.Queue` must precede any
  use of `Queue.newQueue()`.

Stdlib packages are not special-cased at the resolver level. A stdlib package is just
another `.zl` file the loader happens to find via the second root, resolved through the
exact same code path as a project's own `import pkg.Helpers`.

Full lookup precedence is documented in [development.md](development.md#stdlib-lookup).

## External dependencies (`zlpkg`)

Beyond the project root and the stdlib root, `zl_language` searches any number of
additional roots passed as repeatable `--root <path>` flags, or listed in the
`ZL_EXTRA_ROOTS` environment variable (OS path-list separated: `;` on Windows, `:` on
POSIX). They are tried in the order given, before the stdlib root.

`zlpkg` — a separate CLI built alongside `zl_language` from the same `CMakeLists.txt` —
turns a dependency graph into that list of roots automatically:

```bash
# in a project directory containing zlpkg.toml:
zlpkg install            # resolve dependencies, write zlpkg.lock
zlpkg install --update   # force a fresh resolve, ignoring any existing lock
zlpkg run src/Main.zl    # install if needed, then run with dependencies wired in
```

`zlpkg run` also verifies that the runtime next to `zlpkg` reports a matching version.

### `zlpkg.toml`

```toml
[package]
name = "myproject"
version = "0.1.0"

[dependencies]
geom = { path = "../geom" }                                       # local, for dev/fixtures
ecs  = { git = "https://example.com/zl-ecs.git", ref = "v1.0.0" } # ref is a tag/branch/commit
```

Omit `ref` to track the default branch.

### Versioning convention

- Every installable package declares `[package] name` and an exact `version`; the
  project convention is `MAJOR.MINOR.PATCH`.
- A dependency may request an exact version with `version = "..."` in its inline table.
  `zlpkg` verifies the resolved manifest declares exactly that version. There is
  intentionally no version-range or registry resolution yet.
- Package names must agree between the dependency key and the dependency's own manifest.
- `zlpkg.lock` records the resolved version alongside source and ref, so the dependency
  API identity is visible in the lockfile.

### Resolution

Each dependency's own `zlpkg.toml`, if present, is resolved transitively. If the same
dependency name is reached through two different paths in the graph and they resolve to
different content — a different commit, or a different local path — `zlpkg` reports a
version-conflict error naming both requesters rather than silently picking one.

`zlpkg.lock` records the exact resolved commit or path for each dependency and is meant
to be committed, like `Cargo.lock`. Only the `.zlpkg/` cache directory it populates
should be gitignored.

Dependencies are fetched over git by shelling out to the system `git` CLI. No registry
or index exists yet, so every dependency must state exactly where it comes from —
`path` or `git`, never a bare version number.
