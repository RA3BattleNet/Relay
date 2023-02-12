cmake_minimum_required(VERSION 3.16)
project(Relay)

# Dependencies
find_path(CPP_HTTPLIB_INCLUDE_DIRS "httplib.h")
find_package(Boost REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(${PROJECT_NAME})
add_subdirectory("source")
# Macros
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "CPPHTTPLIB_OPENSSL_SUPPORT=1"
)
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
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL OpenSSL::Crypto
)
