@echo off
setlocal enabledelayedexpansion
REM
REM One server and several clients, all on this machine.
REM
REM Each client is admitted, given a blocky character on the spawn pad and told
REM which player it is; WASD walks it and Space jumps. Every client sees every
REM other character move, because the movement happens once - on the server -
REM and what crosses is the intent going up and the transform coming down.
REM
REM   scripts\demos\run-local-server.bat              a server and two clients
REM   scripts\demos\run-local-server.bat 4            four clients instead
REM   set PORT=9100        (then run it)             a different port
REM   set SCENE=Slide.luau (then run it)             a different world
REM   set PRESET=release   (then run it)             the shipped numbers
REM
REM This is not mono.unified_server_client: that harness cuts `net` out of the
REM middle, and this puts the socket, the handshake and the cipher back - and
REM adds the thing neither of them had, more than one player.
REM
REM Each program opens its own window. Close them to stop the demo.

set "here=%~dp0"
set "root=%here%..\.."

if not defined PRESET set "PRESET=dev"
if not defined PORT set "PORT=9099"
if not defined SCENE set "SCENE=Playground.luau"

set "clients=%~1"
if "%clients%"=="" set "clients=2"

set "build=%root%\.cache\build\%PRESET%"

if "%PRESET%"=="server" (
	echo the 'server' preset builds no client - there would be nobody to connect. 1>&2
	exit /b 1
)

call cmake -S "%root%" --preset "%PRESET%" > nul || exit /b 1
call cmake --build "%build%" --target client server || exit /b 1

set "client=%build%\client\client.exe"
set "server=%build%\server\server.exe"

REM The staged copy, not the source - a demo that ran the source tree would work
REM here and nowhere a staged tree was copied to.
set "staged=%build%\client\assets\examples\%SCENE%"
if not exist "%staged%" set "staged=%build%\assets\examples\%SCENE%"
if not exist "%staged%" (
	echo no staged scene at %staged% 1>&2
	exit /b 1
)

echo server: hosting %SCENE% on 127.0.0.1:%PORT%
start "atomic server" "%server%" --game "%staged%" --listen %PORT%

REM A moment before the first client. A UDP connector sent at a port nothing is
REM bound to gets an ICMP refusal, and the retry is slower than simply waiting.
call :pause_briefly

for /l %%i in (1,1,%clients%) do (
	echo client %%i: connecting
	start "atomic client %%i" "%client%" --connect "127.0.0.1:%PORT%" --net --stats
	call :pause_briefly
)

echo.
echo WASD walks, Space jumps, right mouse turns the camera. Close the windows to stop.
exit /b 0

:pause_briefly
REM `timeout` refuses to run when input is redirected, which is what happens
REM inside a build step - `ping` to the loopback is the portable sleep.
ping -n 2 127.0.0.1 > nul
exit /b 0
