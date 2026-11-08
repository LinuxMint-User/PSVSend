# 主机侧纯逻辑单测：不加载 VitaSDK 工具链，直接 gcc/clang 构建。
cmake_minimum_required(VERSION 3.16)
project(psvsend_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

add_executable(test_recv_path
    test_recv_path.c
    ../src/proto/recv_path.c
)
target_include_directories(test_recv_path PRIVATE ../src)
target_compile_options(test_recv_path PRIVATE -Wall -Wextra -Werror)

add_test(NAME recv_path COMMAND test_recv_path)
