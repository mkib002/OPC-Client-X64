cmake_minimum_required(VERSION 3.10)
project(OPCClientDemo)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include_directories(../OPCClientToolKit)
link_directories(../OPCClientToolKit/out/install/x86-Release/lib)

set(SOURCE_CPP OPCClientDemo.cpp)
set(SOURCE_EXE ${SOURCE_H} ${SOURCE_CPP})

if (OPCDACLIENT_STATIC)
  add_definitions(-DNO_OPCDACLIENT_LIBRARY)
endif()

add_executable(OPCClientDemo ${SOURCE_EXE})
target_link_libraries(OPCClientDemo OPCClientToolKit)

install(TARGETS OPCClientDemo DESTINATION bin)
