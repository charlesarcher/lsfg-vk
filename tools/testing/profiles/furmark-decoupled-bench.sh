#!/usr/bin/env bash
exec "$(cd "$(dirname "$0")/.." && pwd)/lsfg-launch.sh" furmark-decoupled-bench
