#!/bin/sh
# Doxygen INPUT_FILTER: convert regular `//` line comments into `///`
# Doxygen-style documentation comments, preserving indentation. Doxygen
# does not treat plain `//` comments as documentation by default, and the
# project uses plain `//` throughout — this filter bridges the gap without
# modifying source files.
#
# Only `//` followed by a space or end-of-line is rewritten, so existing
# `///`, `//!`, and `//<` doc markers (if any) are left alone.

sed -E 's|^([[:space:]]*)// |\1/// |; s|^([[:space:]]*)//$|\1///|' "$1"
