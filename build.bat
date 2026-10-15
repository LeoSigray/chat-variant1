@echo off
rem Build chat server and client on Windows.
rem Uses MinGW/MSYS2 gcc if it is in PATH, otherwise MSVC (cl.exe) -
rem for MSVC run this file from "Developer Command Prompt for VS".
cd /d "%~dp0"

where gcc >nul 2>nul
if %errorlevel%==0 goto gcc
where cl >nul 2>nul
if %errorlevel%==0 goto msvc

echo No C compiler found. Install MSYS2 (gcc) or run from "Developer Command Prompt for VS".
exit /b 1

:gcc
echo Building with gcc...
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o server.exe server.c common.c -lws2_32 || goto fail
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o client.exe client.c common.c -lws2_32 || goto fail
goto ok

:msvc
echo Building with MSVC...
rem /utf-8 is required: string literals must be UTF-8
cl /nologo /utf-8 /W4 /O2 /Fe:server.exe server.c common.c ws2_32.lib || goto fail
cl /nologo /utf-8 /W4 /O2 /Fe:client.exe client.c common.c ws2_32.lib || goto fail
del /q *.obj >nul 2>nul
goto ok

:fail
echo Build FAILED.
exit /b 1

:ok
echo Done: server.exe and client.exe
exit /b 0
