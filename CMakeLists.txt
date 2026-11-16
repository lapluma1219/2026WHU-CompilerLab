cmake_minimum_required(VERSION 3.16)

# 未显式指定编译器时用 g++。部分环境里的 c++ 是无法链接 libstdc++ 的 clang。
if(NOT CMAKE_CXX_COMPILER)
  find_program(_TOYC_GXX g++)
  if(_TOYC_GXX)
    set(CMAKE_CXX_COMPILER "${_TOYC_GXX}")
  endif()
endif()

project(toyc_compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(compiler
  src/main.cpp
  src/lexer/lexer.cpp
  src/lexer/token.cpp
  src/parser/parser.cpp
  src/sema/sema.cpp
)
target_include_directories(compiler PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(compiler PRIVATE -Wall -Wextra -Wpedantic)

# 作业示例在仓库根目录执行 ./compiler。构建目录里保留一份，再复制到根目录。
add_custom_command(
  TARGET compiler POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:compiler> ${CMAKE_SOURCE_DIR}/compiler
  COMMENT "Copy compiler to repository root"
)
