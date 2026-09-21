echo building...
xmake f -c -p windows -a x86 -m release --toolchain=msvc -y
xmake build -v hl2x_console
echo done!