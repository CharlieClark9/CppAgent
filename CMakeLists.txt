cmake_minimum_required(VERSION 3.21)

project(CppAgent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CPPAGENT_FETCH_DEPS "Download header-only dependencies if they are not installed" ON)

include(FetchContent)

add_executable(CppAgent
    CppAgent/src/main.cpp
    CppAgent/src/agent.cpp
    CppAgent/src/tools.cpp
)

target_include_directories(CppAgent
    PRIVATE
        CppAgent/src
)

if(WIN32)
    target_compile_definitions(CppAgent PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    target_compile_options(CppAgent PRIVATE /W4 /permissive-)
else()
    target_compile_options(CppAgent PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(RapidJSON CONFIG QUIET)
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)

if(TARGET RapidJSON::RapidJSON)
    target_link_libraries(CppAgent PRIVATE RapidJSON::RapidJSON)
elseif(RAPIDJSON_INCLUDE_DIR)
    target_include_directories(CppAgent PRIVATE "${RAPIDJSON_INCLUDE_DIR}")
elseif(CPPAGENT_FETCH_DEPS)
    FetchContent_Declare(
        rapidjson
        GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(rapidjson)
    if(NOT rapidjson_POPULATED)
        FetchContent_Populate(rapidjson)
    endif()
    target_include_directories(CppAgent PRIVATE "${rapidjson_SOURCE_DIR}/include")
else()
    message(FATAL_ERROR "RapidJSON was not found. Install it or configure with -DCPPAGENT_FETCH_DEPS=ON.")
endif()

find_package(httplib CONFIG QUIET)

if(TARGET httplib::httplib)
    target_link_libraries(CppAgent PRIVATE httplib::httplib)
elseif(CPPAGENT_FETCH_DEPS)
    FetchContent_Declare(
        cpp-httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cpp-httplib)
    target_link_libraries(CppAgent PRIVATE httplib::httplib)
else()
    message(FATAL_ERROR "cpp-httplib was not found. Install it or configure with -DCPPAGENT_FETCH_DEPS=ON.")
endif()

add_custom_command(
    TARGET CppAgent POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/CppAgent/config.txt"
            "$<TARGET_FILE_DIR:CppAgent>/config.txt"
    COMMENT "Copying default config next to the executable"
)
