# examples/CMakeLists.txt —— 与库同源构建的示例程序（仅帮助理解 API，不参与测试）。

add_executable(hplogc_example_basic basic.c)
target_link_libraries(hplogc_example_basic PRIVATE hplogc)

add_executable(hplogc_example_async async_demo.c)
target_link_libraries(hplogc_example_async PRIVATE hplogc)
