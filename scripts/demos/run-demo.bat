@echo off
REM
REM Runs any staged Luau or TypeScript example through one launcher.
REM
REM A bare stem means Luau. TypeScript scenes are staged as JavaScript, so pass
REM the emitted .js name on Windows. With no scene, the client opens Rings.luau.
REM
REM   scripts\demos\run-demo.bat Terrain --stats
REM   scripts\demos\run-demo.bat Mirrors-1-world.js --stats
REM   scripts\demos\run-demo.bat Mirrors-4-worlds --worlds 4 --stats
REM   set MAX_FPS=60   (then run it) Interface
REM
REM Everything after the optional scene name reaches the client unchanged.

set "SCENE="
set "CLIENT_ARGS=%*"
if not "%~1"=="" if not "%~1:~0,1%"=="-" (
    set "SCENE=%~1"
)

if not "%SCENE%"=="" if "%SCENE:.=%"=="%SCENE%" set "SCENE=%SCENE%.luau"
if not "%SCENE%"=="" for /f "tokens=1,*" %%A in ("%CLIENT_ARGS%") do set "CLIENT_ARGS=%%B"

call "%~dp0_common.bat" "%SCENE%" %CLIENT_ARGS%
exit /b %ERRORLEVEL%
