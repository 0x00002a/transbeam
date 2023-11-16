

find_package(benchmark REQUIRED)

add_executable(bench main.cpp)
setup_exe(bench)

target_link_libraries(bench PRIVATE benchmark::benchmark transbeam)
target_compile_options(bench PRIVATE -fno-omit-frame-pointer)
