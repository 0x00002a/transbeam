

function(add_example NAME SRC)
    add_executable(${NAME} ${SRC})
    target_link_libraries(${NAME} transbeam)
    setup_exe(${NAME})
endfunction()

add_example(basic_bounded basic_bounded.cpp)
add_example(basic_unbounded basic_unbounded.cpp)
