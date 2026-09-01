# Packaging ush

How `ush` gets from source to something installable, and how to test
each packaging format locally before trusting it to a real release.
Everything here builds on one thing: CMake's `install()` rule for the
`ush` target (top-level `CMakeLists.txt`), which just copies the binary
to `<prefix>/bin/ush`. Every format below is a different wrapper around
that same install step.

## Plain install (no package manager)

```bash
cmake -S . -B build -DUSH_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build   # installs to /usr/local/bin/ush by default
```

`--prefix <dir>` on the install line (or `-DCMAKE_INSTALL_PREFIX=<dir>`
at configure time) installs somewhere else instead - useful for testing
without touching the real system:

```bash
cmake --install build --prefix /tmp/ush-test
/tmp/ush-test/bin/ush -c 'echo it works'
```

## CPack: .tar.gz / .pkg (macOS) / .deb + .rpm (Linux)

CPack (configured at the bottom of the top-level `CMakeLists.txt`) turns
the same install() rule into an actual package file. It auto-selects
which generators make sense for the OS `cmake` is currently configuring
for - there's no cross-packaging (building a .deb from macOS, say):

| Platform | Generators   | Needs                                    |
|----------|--------------|-------------------------------------------|
| macOS    | TGZ, productbuild (.pkg) | Xcode Command Line Tools (`pkgbuild`/`productbuild`, both preinstalled) |
| Linux    | TGZ, DEB, RPM | `dpkg-deb` (preinstalled on Debian/Ubuntu), `rpmbuild` (`apt install rpm` / `dnf install rpm-build`) |

To build and inspect a package locally, in a scratch build directory
(so this never touches your normal dev `build/`):

```bash
cmake -S . -B build-pkg -DUSH_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-pkg -j 8
cd build-pkg && cpack
```

This produces `ush-<version>-<system>.*` files right in `build-pkg/`.
Two things worth knowing if you ever touch the CPack config:

- **The macOS `.pkg`'s install prefix defaults to `/Applications`**,
  which is right for a `.app` bundle and useless for a CLI tool (the
  binary would land at `/Applications/bin/ush`, nowhere near `$PATH`).
  `CPACK_PACKAGING_INSTALL_PREFIX` is explicitly set to `/usr/local` to
  fix this - verify a real `.pkg`'s payload lands at `usr/local/bin/ush`
  (not `Applications/...`) with `pkgutil --expand-full <pkg> <dir>` and
  `find <dir>` after any change here, since this is easy to silently
  break again.
- **`CPACK_RESOURCE_FILE_LICENSE` can't just point at `LICENSE.md`** -
  the macOS `productbuild` generator rejects anything but `.rtfd`/
  `.rtf`/`.html`/`.txt` outright ("Bad file extension specified"). A
  `.txt` copy is generated into the build tree at configure time
  (`configure_file(... COPYONLY)`) and used for every generator instead
  of branching per-platform - it's never checked into the repo.
- **`productbuild` needs `CPACK_COMPONENTS_ALL` set even for a single,
  uncomponentized `install()` rule**, or the generated Distribution XML
  ends up with an empty `<choices-outline/>` - no `<choice>`/`<pkg-ref>`
  at all - which leaves Installer.app with nothing selected and nothing
  *selectable*, so its Install button stays permanently greyed out with
  no way to proceed. `CPACK_COMPONENTS_ALL Unspecified` (CPack's own
  name for the implicit component an unlabeled `install()` lands in)
  fixes it. This one only showed up by actually opening the real `.pkg`
  in Installer.app and clicking through it - inspecting the payload with
  `pkgutil --expand-full` (below) doesn't catch it, since the payload
  itself is completely correct; it's the installer *UI* that's broken.
  After touching anything CPack/productbuild-related, click through the
  real installer at least once, not just `pkgutil --expand-full` it.

Verifying a `.pkg`/`.deb`/`.rpm` payload without actually installing it
system-wide:

```bash
# macOS - expands the package into a plain directory tree, no install
pkgutil --expand-full build-pkg/ush-*.pkg /tmp/pkg-expand
find /tmp/pkg-expand -type f

# Linux .deb - lists (and can extract) the payload without installing
dpkg-deb --contents build-pkg/ush-*.deb
dpkg-deb -x build-pkg/ush-*.deb /tmp/deb-expand

# Linux .rpm
rpm2cpio build-pkg/ush-*.rpm | cpio -idmv -D /tmp/rpm-expand
```

## GitHub Releases (prebuilt binaries)

`.github/workflows/release.yml` builds and packages `ush` on a macOS
(Apple Silicon) and a Linux runner, then attaches whatever CPack
produced (`.tar.gz`/`.pkg`/`.deb`/`.rpm`) to a GitHub Release - triggered
by pushing a tag matching `v*` (e.g. `v0.1.0`). To cut a release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The workflow itself can't be exercised locally (no Docker-based GitHub
Actions runner - e.g. `act` - was available when this was built); review
it carefully before trusting a real tag push, and check the Actions tab
after pushing one.

## Homebrew formula

`Formula/ush.rb` builds `ush` from source via the same CMake steps as
everywhere else. It currently has **only a `head` block** (builds from
the tip of `main`) - there's no tagged release yet to point a stable
`url`/`sha256` at. Test it locally with:

```bash
brew install --HEAD --formula Formula/ush.rb
brew test ush
brew uninstall ush   # when done testing
```

Two details worth knowing if you touch this formula:

- `-DUSH_BUILD_TESTS=OFF` is not optional here: Homebrew's build
  sandbox has no network access, but leaving tests on would make CMake
  try to `FetchContent` Catch2 from GitHub mid-build and fail outright.
- `*std_cmake_args` (Homebrew's own helper) already supplies
  `-DCMAKE_INSTALL_PREFIX=<the right Cellar path>` and
  `-DCMAKE_BUILD_TYPE=Release` - don't set either by hand, or a formula
  update elsewhere in Homebrew could silently stop taking effect.

### Adding the stable block once v0.1.0 (or later) is tagged and pushed

```bash
curl -sL -o /tmp/ush.tar.gz \
  https://github.com/jacobStuff/unnamed-shell/archive/refs/tags/v0.1.0.tar.gz
shasum -a 256 /tmp/ush.tar.gz
```

Add to the formula, above the existing `head` line:

```ruby
url "https://github.com/jacobStuff/unnamed-shell/archive/refs/tags/v0.1.0.tar.gz"
sha256 "<the shasum output above>"
version "0.1.0"
```

Then `brew install --formula Formula/ush.rb` (no `--HEAD`) exercises the
stable path.

### Publishing it properly (a personal tap)

A formula living in this repo only installs via `--formula path/to/it`.
To get plain `brew install ush` working, it needs to live in its own
`homebrew-<name>` repo (Homebrew's "tap" convention) - e.g. a repo named
`homebrew-ush` under the same GitHub account, containing just
`Formula/ush.rb`. Once that exists:

```bash
brew tap jacobstuff/ush
brew install ush
```
