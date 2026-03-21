cmake_minimum_required(VERSION 3.15)  
project(TinyCoroutine                     
  VERSION 1.0.0                        
  LANGUAGES CXX)                      

set(CMAKE_CXX_STANDARD 20)             
set(CMAKE_CXX_STANDARD_REQUIRED ON)   
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Wall -Wextra -g -O0)  # Debug 配置
else()
  add_compile_options(-O3 -DNDEBUG)          # Release 配置
endif()

include_directories(include)           # 公共头文件目录

# 查找 liburing
find_library(URING_LIB uring)
if(NOT URING_LIB)
  message(FATAL_ERROR "liburing not found. Please install liburing-dev")
endif()
