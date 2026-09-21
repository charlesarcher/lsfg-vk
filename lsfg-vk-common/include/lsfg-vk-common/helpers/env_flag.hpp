/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdlib>

/// S42o (debug-gate semantics): an env flag is on only when it is set
/// to a non-zero, non-empty value. This kills the "LSFGVK_*_DBG=0 is
/// still enabled because getenv()!=nullptr" class of hot-path logging
/// bugs — every gate in app/common/layer that used to test bare
/// getenv() goes through this helper, so "VAR=0" is a real OFF.
///
/// Performance: getenv() is O(flag length) table scan, cached by glibc
/// after first hit (fast path); the branch is perfectly predicted; it
/// replaces identical-if-not-cheaper bare checks. Never call this
/// inside per-pixel loops — cache it in a static (the per-frame gates
/// already do).
inline bool envFlagOn(const char* name) {
    const char* v = ::getenv(name);
    if (v == nullptr)
        return false;
    return !(v[0] == '0' || v[0] == '\0');
}
