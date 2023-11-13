
add_executable(tests main.cpp)

find_package(doctest REQUIRED)

target_link_libraries(tests PUBLIC transbeam doctest::doctest)

add_test(NAME tests COMMAND tests)
