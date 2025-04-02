@echo off
REM Check if the "build" folder exists
if exist build (
    REM Delete the "build" folder and all its contents
    rmdir /s /q build
    echo "build folder deleted."
)
set CC=gcc
set CXX=g++
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make