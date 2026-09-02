@echo off
rem Build the DIAGNOSTIC variant: logging forced on, the extra capture-context
rem line compiled in, and the version string stamped "1.1.5-diag1" so any log it
rem produces identifies itself. For handing to one reporter, never for release.
rem
rem TWO THINGS THIS DELIBERATELY DOES NOT SHARE WITH build.bat:
rem   1. Its own build tree (build\diag), so the release tree in build\release
rem      keeps the bytes that were packaged and field-tested.
rem   2. Its own OUTPUT_FOLDER, so the POST_BUILD deploy CANNOT write a
rem      diagnostic DLL into the game. build.bat deploys itself; this one has to
rem      be copied by hand, on purpose.
call :main > "%~dp0build_diag.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."
echo === CONFIGURE START ===
rem !! QUOTE THE DEPLOY PATH. Unquoted, %CD% splits on the space in
rem "C:\Studios\Mod Studio\Menu Studio" and CMake receives "C:\Studios\Mod",
rem which it happily deploys a whole payload into: a stray C:\Studios\Mod tree
rem appeared the first time this ran. Forward slashes as well, so CMake is never
rem handed a backslash to interpret as an escape.
"%CM%" --preset release -B build/diag -DMENUSTUDIO_DIAG=ON -DMENUSTUDIO_TESTS=OFF "-DOUTPUT_FOLDER=%CD%/build/diag-deploy"
if errorlevel 1 goto :fail
echo === BUILD START ===
"%CM%" --build build/diag
if errorlevel 1 goto :fail
echo === DIAG_DONE ===
exit /b 0

:fail
echo ***DIAG_BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
