cmake_minimum_required(VERSION 3.20)
project(s6_dexel CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(s6dexel main.cpp)
if(MSVC)
  # Default x64 code generation (SSE2), /fp:precise. No /arch:AVX2 so the numbers do not assume a new CPU.
  target_compile_options(s6dexel PRIVATE /W3 /O2 /fp:precise)
endif()
