#!/system/bin/sh

trace=/data/local/tmp/hg.attach.trace
log=/data/local/tmp/hg.attach-watch.log

rm -f "$trace" "$log"

pid=""
while [ -z "$pid" ]; do
    pid="$(pidof com.com2us.HG 2>/dev/null)"
done

echo "attaching pid=$pid" > "$log"
exec /data/local/tmp/hg-strace64 -f -tt -s 256 -o "$trace" -p "$pid" >> "$log" 2>&1
