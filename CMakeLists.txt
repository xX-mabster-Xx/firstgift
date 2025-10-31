cmake_minimum_required(VERSION 3.11)
project(tg_gifts)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(spdlog REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

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

add_executable(tg_gifts
    src/main.cpp
    src/td_interface.cpp
)


target_link_libraries(tg_gifts PRIVATE
    cpr::cpr
    spdlog::spdlog
    Td::TdStatic
	nlohmann_json::nlohmann_json
    pthread
    z
    ssl
    crypto
)
