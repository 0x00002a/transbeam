cmake_minimum_required(VERSION 3.5)
project(transbeam)

option(TRANSBEAM_BUILD_TESTS "Build tests" ON)
option(TRANSBEAM_BUILD_EXAMPLES "Build examples" OFF)

set(CMAKE_CXX_STANDARD 20)

function(setup_exe NAME)
   # enable colour output with ninja
   if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
      target_compile_options(${NAME} PRIVATE -fdiagnostics-color=always)
   elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL Clang)
      target_compile_options(${NAME} PRIVATE -fcolor-diagnostics)
   endif ()
   if (${CMAKE_CXX_COMPILER_ID} MATCHES "(GNU|Clang|AppleClang)")
      target_compile_options(${NAME} PRIVATE
        -Wall
        -Werror
        -Wextra
        -Wpedantic
        -Wconversion

        -Wno-error=unused-but-set-variable
        -Wno-error=unused-variable
        -Wno-error=unused-parameter
        -Wno-error=unused-function

      )
   endif()
   if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
      target_compile_options(${NAME} PRIVATE
        -Warith-conversion
      )
   endif()

endfunction()


file(GLOB_RECURSE SRC "${CMAKE_CURRENT_LIST_DIR}/include/*.hpp")


add_library(transbeam INTERFACE ${SRC})
target_include_directories(transbeam INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")

if (${TRANSBEAM_BUILD_TESTS})
    add_subdirectory(tests)
endif()

if (${TRANSBEAM_BUILD_EXAMPLES})
   add_subdirectory(examples)
endif()
