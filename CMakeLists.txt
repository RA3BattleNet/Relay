cmake_minimum_required(VERSION 3.20)
project(Relay)

# Dependencies
find_path(CPP_HTTPLIB_INCLUDE_DIRS "httplib.h")
find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)

add_executable(${PROJECT_NAME})
add_subdirectory("source")
# UTF-8
target_compile_options(${PROJECT_NAME} PRIVATE "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
# C++20
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
# Dependencies
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CPP_HTTPLIB_INCLUDE_DIRS}
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::boost
    spdlog::spdlog
)
