@echo off
:: This file is used to invoke nanopb_generator.py as a plugin
:: to protoc on Windows.
:: Use it like this:
:: protoc --plugin=protoc-gen-nanopb=..../protoc-gen-nanopb.bat --nanopb_out=dir foo.proto
::
:: Note that if you use the binary package of nanopb, the protoc
:: path is already set up properly and there is no need to give
:: --plugin= on the command line.

set mydir=%~dp0
set "nanopb_python=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
if not exist "%nanopb_python%" (
  where python >NUL 2>&1
  if errorlevel 1 (
    echo nanopb plugin launcher: python executable not found in PATH. 1>&2
    exit /b 9009
  )
  set "nanopb_python=python"
)

set "_nanopb_errlog=%TEMP%\nanopb_plugin_%RANDOM%_%RANDOM%.err"
"%nanopb_python%" "%mydir%\nanopb_generator.py" --protoc-plugin %* 2>"%_nanopb_errlog%"
set "_nanopb_ec=%ERRORLEVEL%"
if not "%_nanopb_ec%"=="0" (
  echo nanopb plugin launcher failed command: "%nanopb_python%" "%mydir%\nanopb_generator.py" --protoc-plugin %* 1>&2
  if exist "%_nanopb_errlog%" type "%_nanopb_errlog%" 1>&2
)
if exist "%_nanopb_errlog%" del /q "%_nanopb_errlog%" >NUL 2>&1
exit /b %_nanopb_ec%
