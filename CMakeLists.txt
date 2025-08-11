cmake_minimum_required(VERSION 3.11)
project(tg_ai_agent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(spdlog REQUIRED)

include(FetchContent)

set(CPR_USE_SYSTEM_CURL ON)
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/libcpr/cpr.git
  GIT_TAG        1.11.2
)

FetchContent_MakeAvailable(cpr)


# add_subdirectory(td)
find_package(Td REQUIRED)


include_directories(
    include
)

add_executable(tg_ai_agent
    src/main.cpp
    src/td_interface.cpp
)


target_link_libraries(tg_ai_agent PRIVATE
    cpr::cpr
    spdlog::spdlog
    Td::TdStatic
    pthread
    z
    ssl
    crypto
)
