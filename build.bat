@echo off
setlocal

set CC=C:\Users\Pongo\Documents\mingw64\bin\gcc.exe
set CXX=C:\Users\Pongo\Documents\mingw64\bin\g++.exe

echo Compiling MinHook...
%CC% -c -O2 -Iminhook/include minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde64.c
if errorlevel 1 (
    echo MinHook compilation failed.
    exit /b 1
)

echo Compiling FFRS-Turbo dwmapi.dll...
%CXX% -shared -O2 -static -o dwmapi.dll src/main.cpp buffer.o hook.o trampoline.o hde64.o -Iminhook/include -lgdiplus -lgdi32 -s
if errorlevel 1 (
    echo dwmapi.dll build failed.
    del *.o >nul 2>&1
    exit /b 1
)

del *.o >nul 2>&1
echo Build successful: dwmapi.dll created.
endlocal
