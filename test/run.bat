@echo off
rem Builds the test generators with MSVC, emits the Windows objects and the C output, and
rem runs smoke, abi and cgen against both. Run it from the repository root; it calls
rem vcvars64.bat itself when cl is not on the path. The outputs go to build\msvc.
setlocal
where cl >nul 2>nul || call :vcvars || exit /b 2
set O=build\msvc
if not exist %O% mkdir %O%
set failed=0

echo building the generators
for %%t in (smoke abi cgen) do (
	cl /nologo /std:c11 /W3 /DTG_CHECK_IR /Iinclude test\%%t.c src\tinygen.c /Fo%O%\ /Fe:%O%\%%t.exe || exit /b 1
)
cl /nologo /std:c11 /O2 /W3 /DTG_CHECK_IR /Iinclude test\bench.c src\tinygen.c /Fo%O%\ /Fe:%O%\bench.exe || exit /b 1

echo emitting the objects and the C
for %%t in (smoke abi cgen) do (
	%O%\%%t.exe %O%\%%t.obj windows || call :fail emit %%t
	%O%\%%t.exe %O%\%%t-c.c c || call :fail emit %%t-c
)
%O%\bench.exe %O%\bench.obj windows 2000 2000 >nul || call :fail emit bench
%O%\bench.exe %O%\bench-c.c c 2000 2000 >nul || call :fail emit bench-c

echo running the tests against the objects
cl /nologo /Zi /Od test\smoke_main.c %O%\smoke.obj /Fo%O%\ /Fd%O%\ /Fe:%O%\smoke_run.exe /link /DEBUG:FULL && %O%\smoke_run.exe || call :fail smoke
cl /nologo /std:c11 /Zi /Od test\abi_main.c %O%\abi.obj /Fo%O%\ /Fd%O%\ /Fe:%O%\abi_run.exe /link /DEBUG:FULL && %O%\abi_run.exe || call :fail abi
cl /nologo /std:c11 /Zi /Od test\cgen_main.c %O%\cgen.obj /Fo%O%\ /Fd%O%\ /Fe:%O%\cgen_run.exe /link /DEBUG:FULL && %O%\cgen_run.exe || call :fail cgen

echo running the tests against the C output
for %%o in (Od O2) do (
	cl /nologo /std:c11 /W3 /%%o %O%\smoke-c.c test\smoke_main.c /Fo%O%\ /Fe:%O%\smoke_c_%%o.exe && %O%\smoke_c_%%o.exe || call :fail smoke c /%%o
	cl /nologo /std:c11 /W3 /%%o /DTG_FAR_COUNT=2500 %O%\abi-c.c test\abi_main.c /Fo%O%\ /Fe:%O%\abi_c_%%o.exe && %O%\abi_c_%%o.exe || call :fail abi c /%%o
	cl /nologo /std:c11 /W3 /%%o %O%\cgen-c.c test\cgen_main.c /Fo%O%\ /Fe:%O%\cgen_c_%%o.exe && %O%\cgen_c_%%o.exe || call :fail cgen c /%%o
	cl /nologo /std:c11 /W3 /%%o /c %O%\bench-c.c /Fo%O%\bench-c.obj || call :fail bench c /%%o
)

if %failed% neq 0 (
	echo some tests FAILED
	exit /b 1
)
echo all passed
exit /b 0

:fail
echo FAILED: %*
set failed=1
exit /b 0

:vcvars
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
	echo cl is not on the path and vswhere.exe is missing: run from an x64 Native Tools prompt
	exit /b 1
)
for /f "usebackq delims=" %%p in (`"%vswhere%" -latest -products * -property installationPath`) do set "vs=%%p"
if not defined vs (
	echo cl is not on the path and vswhere found no Visual Studio
	exit /b 1
)
call "%vs%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
