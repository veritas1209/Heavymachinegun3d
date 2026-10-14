#!/system/bin/sh
exec /data/local/tmp/hg-strace64 -f -tt -s 256 -o /data/local/tmp/hg.trace "$@"
