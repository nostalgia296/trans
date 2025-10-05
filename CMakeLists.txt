# 设置CMake的最低版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(TranslationWorker)

# 设置C++标准为C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 添加可执行文件
add_executable(translation_worker trans.cpp)

# 添加链接的库
target_link_libraries(translation_worker PRIVATE curl jsoncpp)

# 添加编译选项
target_compile_options(translation_worker PRIVATE -O3 -flto -pthread)
