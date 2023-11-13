
add_library(tests main.cpp)

find_package(doctest REQUIRED)

target_link_libraries(tests PUBLIC transbeam doctest)

add_test(NAME tests COMMAND tests)
