#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Steam launch option. This path stays in Steam's command line box.
# It does not start a doubler and it does not reuse one.
# One path: the re2 profile.
#
#   /path/to/lsfg-vk/tools/games/launch_re2.sh %command%

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 %command%"
  echo "Calls tools/testing/profiles/re2.sh."
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$REPO_ROOT/tools/testing/profiles/re2.sh" "$@"
