cmake_minimum_required(VERSION 3.14)
project(zip_extract)

include(FetchContent)

FetchContent_Declare(
  zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
)

FetchContent_MakeAvailable(zlib)

add_executable(zip_extract main.c)

target_link_libraries(zip_extract PRIVATE zlib)

message(STATUS "Zlib DLL Path: $<TARGET_FILE:zlib>")

add_custom_command(TARGET zip_extract POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:zlib> $<TARGET_FILE_DIR:zip_extract>
)