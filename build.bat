@echo off
rem ============================================================
rem  编译 Windows 发送端 -> build\Release\fenghua_sender.exe
rem
rem  需要 Visual Studio 2022 (含 C++ 桌面开发) 或 Build Tools
rem  FFmpeg 开发包默认找 D:\fenghua_build\ffmpeg_dev
rem    换路径: 本文件里改 FFMPEG_DIR, 或命令行 -DFFMPEG_DIR=...
rem    找不到时只编译键鼠功能 (投屏不可用, 会有 CMake 警告)
rem ============================================================
setlocal
set "FFMPEG_DIR=D:\fenghua_build\ffmpeg_dev"
cd /d "%~dp0"

cmake -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_DIR="%FFMPEG_DIR%"
if %ERRORLEVEL% NEQ 0 (
  echo.
  echo [ERROR] CMake 配置失败。
  echo         没有 VS2022 的话可以试: cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
  exit /b 1
)

cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 exit /b 1

rem 运行时 DLL 放到 exe 同目录
copy /y third_party\*.dll build\Release\ >nul 2>&1

echo.
for %%f in (build\Release\fenghua_sender.exe) do echo [OK] %%~ff  (%%~zf bytes)
echo.
echo 用法: build\Release\fenghua_sender.exe ^<设备IP^> [--stream] [--obs] [--dump 目录]
endlocal
