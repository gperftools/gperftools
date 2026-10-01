#!/usr/bin/env ruby
# -*- Mode: ruby -*-
#
# Install what the other ci/ scripts need, via apt.
#
#   ci/install-deps.rb
#
# Deliberately explicit rather than relying on what a runner image happens
# to preinstall: GitHub's ubuntu-24.04 image carries most of this already,
# act's container images carry much less, and a local box is anyone's
# guess. Installing it in one place is what makes the three behave alike.
#
# This is apt-only, so it assumes a Debian/Ubuntu userland -- which is what
# the workflow runs on. On a box that already has the packages it is a
# no-op, so running it is optional rather than intrusive.
#
# `bazel` is not here: ci/install-bazelisk.rb downloads bazelisk, since no
# apt package honours .bazelversion.

require_relative 'lib'

# qemu linux-user needs: a compiler, git to fetch it, ninja + pkg-config +
# glib for its build system, and python3 with venv because configure builds
# itself a virtualenv to run meson out of.
#
# v/pstepper needs: ninja, ruby (already running), glib headers for the TCG
# plugin, and nothing else.
#
# The bazel sweep needs clang as well, since it builds every configuration
# under both compilers (see ci/run-bazel.rb).
#
# libc6-dbg is not optional for the stepped tests, and the reason is
# indirect: the comparer suppresses its known-bogus spots by *symbol name*
# (`_dl_fixup`, `call_init`, `__run_exit_handlers`), and two of those checks
# look at the source filename as well. glibc's dynamic symbol table does not
# carry those names -- without debug info addr2line answers with the nearest
# exported symbol, which is something else entirely (`_dl_fatal_printf` for
# the lazy-binding path on Ubuntu 24.04), the suppression misses, and a
# lazy-binding truncation gets reported as a real mismatch. It also makes a
# genuine failure report legible instead of a column of `(:0)`.
PACKAGES = %w[
  build-essential
  clang
  curl
  git
  libc6-dbg
  libglib2.0-dev
  ninja-build
  pkg-config
  python3
  python3-venv
]

CI.install_packages!(PACKAGES)
