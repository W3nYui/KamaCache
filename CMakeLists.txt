# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(KCacheSystem)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ==========================================
# Windows/MSVC/GCC 平台特定设置
# ==========================================
if(MSVC)
    # MSVC 编译器选项
    add_compile_options(/utf-8 /W3)
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    # MinGW (GCC) / Clang 编译器选项
    add_compile_options(-Wall -Wextra -O2)
endif()

# 添加当前目录到包含路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 归查找所有 .cpp 文件
file(GLOB_RECURSE ALL_SOURCES "*.cpp")

# 过滤掉构建目录 (build) 下的临时文件
set(SOURCES "")
foreach(src ${ALL_SOURCES})
    # 检查文件路径是否以构建目录(CMAKE_BINARY_DIR)开头
    string(FIND "${src}" "${CMAKE_BINARY_DIR}" BUILD_DIR_POS)
    
    # 只有当文件不在 build 目录下时，才加入编译列表
    if(NOT BUILD_DIR_POS EQUAL 0)
        list(APPEND SOURCES ${src})
    endif()
endforeach()

# 打印调试信息，让你知道具体编译了哪些文件
message(STATUS "Compiling Sources: ${SOURCES}")

# 设置目标可执行文件
add_executable(main ${SOURCES})

# 清理中间的 .o 文件
set_target_properties(main PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# 额外的编译选项（可根据需要启用）
# target_compile_options(main PRIVATE -Wall -Wextra -O2)