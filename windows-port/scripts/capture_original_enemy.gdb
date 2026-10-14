set pagination off
set confirm off
set architecture arm
file /mnt/c/Users/hajin/IT_Projects/android\ emul/windows-port/extracted/apk/lib/armeabi/libgame.so
target remote 172.17.0.1:5041
set $base = 0xa2c33000
break *($base + 0x5b4f0)
continue
set $action = $r0
set $stream = $r1
printf "VERTEX_LOADER action=%#x stream=%#x mode=%u\n", $action, $stream, $r2
x/64bx $stream
finish
printf "VERTEX_RETURN=%#x consumed=%u\n", $r0, $r0-$stream
x/40wx $action
set $action_end = $action + 0x100
dump binary memory "/mnt/c/Users/hajin/IT_Projects/android emul/windows-port/analysis/native/live-vertex-action.bin" $action $action_end
detach
quit
