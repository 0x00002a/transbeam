cmake_minimum_required(VERSION 3.5)
project(transbeam)

option(TRANSBEAM_BUILD_TESTS "Build tests" ON)

set(CMAKE_CXX_STANDARD 20)

# enable colour output with ninja
if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
   add_compile_options(-fdiagnostics-color=always)
elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL Clang)
   add_compile_options(-fcolor-diagnostics)
endif ()


file(GLOB_RECURSE SRC "${CMAKE_CURRENT_LIST_DIR}/include/*.hpp")


add_library(transbeam INTERFACE ${SRC})
target_include_directories(transbeam INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")

if (${TRANSBEAM_BUILD_TESTS})
    add_subdirectory(tests)
endif()