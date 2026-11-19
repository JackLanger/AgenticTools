cmake_minimum_required(VERSION 4.3)
project(AgenticTools)

set(CMAKE_CXX_STANDARD 20)

add_executable(AgenticTools
        src/main.cpp)

target_link_libraries(AgenticTools PRIVATE user32)

find_package(OpenCV REQUIRED)
target_link_libraries(AgenticTools PRIVATE ${OpenCV_LIBS})
