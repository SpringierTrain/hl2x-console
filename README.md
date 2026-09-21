# hl2x_console
a console for the xbox port of Half-Life 2

## building

use the build.bat file, but make sure you fill the requirements needed below.

### Linux

* At least GCC 14
* [xmake](https://xmake.io)
* then run:
```
xmake f --toolchain=gcc-14 (replace if you're using a later version of gcc)
xmake
```

### Windows

* VS2022 build tools (or later)
* [xmake](https://xmake.io)
* then run `xmake`

## running

* put the dll will your xbes (default.xbe, hl2_xbox.xbe). only needs to be done once
* open cxbx-reloaded (ideally a version that supports hl2x, such as [this](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/releases/download/CI-97bf1d9/CxbxReloaded-Release-VS2019.zip) one)
* after loading your game, wait until you see the screen that says "Press Start" 
* run `rundll32 hl2x_console.dll,Run` via cmd in the directory that has your xbes

## credits

made by [fishywitch](https://github.com/fishywitch)