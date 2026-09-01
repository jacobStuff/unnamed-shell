cmake_minimum_required(VERSION 3.20)

project(ush
    VERSION 0.1.0
    DESCRIPTION "ush - unnamed shell, a POSIX-compatible shell"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

option(USH_BUILD_TESTS "Build ush unit/integration tests" ON)

add_subdirectory(src)

if(USH_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.6.0
    )
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()

# --- Installation / packaging -------------------------------------------
#
# `cmake --install build` puts just the `ush` binary somewhere standard
# (GNUInstallDirs picks the right convention per platform - "bin" on both
# macOS and Linux). This is the foundation every packaging format below
# builds on: a Homebrew formula, a Debian/RPM package, and a macOS .pkg
# all ultimately just run this install step and archive the result -
# see docs/PACKAGING.md.

include(GNUInstallDirs)

install(TARGETS ush
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- CPack: turns the install() rule above into an actual package file
# (.deb/.rpm on Linux, a .pkg on macOS, a plain .tar.gz everywhere) - see
# docs/PACKAGING.md for how each is actually built/tested. Only the
# generators relevant to the platform CMake is currently configuring for
# are listed; cross-compiling to package for a different OS than the one
# running cmake isn't supported here (each format's tooling - dpkg-deb,
# rpmbuild, productbuild - is only expected to exist on its own OS
# anyway).

set(CPACK_PACKAGE_NAME "ush")
set(CPACK_PACKAGE_VENDOR "Jacob Ferrari")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jacobStuff/unnamed-shell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# CPack's macOS generator (productbuild) rejects a .md license file
# outright ("Bad file extension... only .rtfd, .rtf, .html, and .txt
# files allowed") - LICENSE.md's content is already plain text with no
# real Markdown formatting, so a byte-for-byte .txt copy in the build
# tree (regenerated on every configure, never checked in) satisfies
# every generator uniformly instead of branching per-platform.
configure_file(${CMAKE_SOURCE_DIR}/LICENSE.md ${CMAKE_BINARY_DIR}/LICENSE.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

set(CPACK_GENERATOR "TGZ")

if(APPLE)
    list(APPEND CPACK_GENERATOR "productbuild")
    # productbuild's package/bundle identifier - reverse-DNS-style, no
    # real domain needed for a personal/OSS project (the same convention
    # e.g. Homebrew-cask-adjacent .pkg installers use).
    set(CPACK_PACKAGE_IDENTIFIER "io.github.jacobstuff.ush")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "${CPACK_PACKAGE_IDENTIFIER}")
    # CPack's own default install prefix for this generator is
    # /Applications (sensible for a .app bundle, useless for a CLI tool -
    # it would land at /Applications/bin/ush, nowhere near $PATH).
    # /usr/local is on macOS's default PATH and is the same convention
    # Homebrew itself uses on Intel Macs; the installer prompts for
    # authorization if that requires elevated permissions, same as any
    # other .pkg that writes outside the user's home directory.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    # Without this, productbuild's generated Distribution XML ends up
    # with an empty <choices-outline/> - no <choice>/<pkg-ref> for the
    # single (uncomponentized) install() rule at all - which leaves
    # Installer.app with nothing selected and no way to select anything,
    # so its Install button stays permanently greyed out. CPack's
    # `install(TARGETS ush)` rule has no explicit COMPONENT, so
    # everything lands in the implicit "Unspecified" component; naming
    # it here is what makes productbuild actually advertise it as a
    # choice. Caught by installing the real .pkg through Installer.app,
    # not by cpack's own (silent, exit-0) output - see docs/PACKAGING.md.
    set(CPACK_COMPONENTS_ALL Unspecified)
elseif(UNIX)
    list(APPEND CPACK_GENERATOR "DEB" "RPM")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "shells")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

    set(CPACK_RPM_PACKAGE_GROUP "System/Shells")
    # SPDX identifier for LICENSE.md's actual text (the Unlicense/public
    # domain dedication) - README.md currently says "TBD (likely GPL)",
    # which is stale; flagged separately rather than silently rewritten.
    set(CPACK_RPM_PACKAGE_LICENSE "Unlicense")
endif()

include(CPack)
