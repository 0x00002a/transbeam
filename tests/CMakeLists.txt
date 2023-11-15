
add_executable(tests main.cpp)
setup_exe(tests)

find_package(doctest REQUIRED)

target_link_libraries(tests PUBLIC transbeam doctest::doctest)


if (${CMAKE_CXX_COMPILER_ID} MATCHES "GNU|Clang")
    target_compile_options(tests PRIVATE -fsanitize=undefined -fsanitize=address)
    target_link_libraries(tests PRIVATE asan ubsan)
endif()
add_test(NAME tests COMMAND tests)
