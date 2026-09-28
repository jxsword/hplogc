# tests/CMakeLists.txt —— 单元测试与冒烟测试。

# 测试需要访问内部头（ring.h / hplogc_internal.h）及其依赖
set(HPLOGC_TEST_INC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/atomic
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/ring
    ${CMAKE_SOURCE_DIR}/include)

add_executable(test_ring ring_test.c)
target_include_directories(test_ring PRIVATE ${HPLOGC_TEST_INC})
target_link_libraries(test_ring PRIVATE hplogc)

add_executable(test_config config_test.c)
target_include_directories(test_config PRIVATE ${HPLOGC_TEST_INC})
target_link_libraries(test_config PRIVATE hplogc)

add_executable(test_smoke smoke_test.c)
target_include_directories(test_smoke PRIVATE ${HPLOGC_TEST_INC})
target_link_libraries(test_smoke PRIVATE hplogc)

enable_testing()
add_test(NAME test_ring COMMAND test_ring)
add_test(NAME test_config COMMAND test_config)
add_test(NAME test_smoke COMMAND test_smoke)
