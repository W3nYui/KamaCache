# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 3.12)

# 设置项目名称
project(KCacheSystem-change)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# CONFIGURE_DEPENDS: 确保 VSCode 在新建文件时能自动刷新
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# 过滤掉构建目录 (build) 下的文件
set(SOURCES "")
foreach(src ${ALL_SOURCES})
    # 检查文件路径是否以构建目录(CMAKE_BINARY_DIR)开头
    string(FIND "${src}" "${CMAKE_BINARY_DIR}" BUILD_DIR_POS)
    
    # 只有当文件不在 build 目录下时 (返回值不为 0)，才加入最终编译列表
    if(NOT BUILD_DIR_POS EQUAL 0)
        list(APPEND SOURCES ${src})
    endif()
endforeach()

# 打印调试信息
message(STATUS "Compiling Sources (Filtered): ${SOURCES}")

# 使用过滤后的 SOURCES 列表创建可执行文件
add_executable(main ${SOURCES})

# 3平台适配与编译选项
if(MSVC)
    # Windows / Visual Studio
    target_compile_options(main PRIVATE /utf-8 /W3)
    target_compile_definitions(main PRIVATE -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    # Linux / GCC / Clang / MinGW
    target_compile_options(main PRIVATE -Wall -Wextra -O2)
    # 如果你在 Linux 下用到多线程 (如 std::thread)，通常需要链接 pthread
    find_package(Threads REQUIRED)
    target_link_libraries(main PRIVATE Threads::Threads)
endif()

# 添加当前目录到包含路径
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 清理中间的 .o 文件 (可选属性)
set_target_properties(main PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# 额外的编译选项（可根据需要启用）
# target_compile_options(main PRIVATE -Wall -Wextra -O2)