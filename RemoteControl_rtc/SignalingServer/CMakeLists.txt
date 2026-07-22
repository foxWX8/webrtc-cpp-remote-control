cmake_minimum_required(VERSION 3.16)
project(RemoteControlSignalingServer LANGUAGES CXX)

add_executable(SignalingServer main.cpp)
target_compile_features(SignalingServer PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(SignalingServer PRIVATE /W4 /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(SignalingServer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(SignalingServer PRIVATE ws2_32)
endif()
