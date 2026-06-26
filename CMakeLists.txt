cmake_minimum_required(VERSION 3.16)

# SDKTARGETSYSROOT 환경변수 지원 (Yocto SDK 크로스 컴파일)
# CMAKE_SYSROOT는 project() 전에 설정해야 컴파일러에 반영됨
if(DEFINED ENV{SDKTARGETSYSROOT})
    set(SDKTARGETSYSROOT $ENV{SDKTARGETSYSROOT})
    set(CMAKE_SYSROOT ${SDKTARGETSYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${SDKTARGETSYSROOT})
else()
    set(SDKTARGETSYSROOT "" CACHE PATH "Yocto SDK target sysroot path")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

project(http_file_server C CXX)

find_package(Threads REQUIRED)

set(SRCS
    src/main.cpp
    src/http_file_server.cpp
    src/http_session.cpp
    src/udp_broadcast_server.cpp
)

add_executable(http_file_server ${SRCS})

target_compile_options(http_file_server PRIVATE -Wall -O2 -g -Wno-unused-result)

# 인클루드 경로
target_include_directories(http_file_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio_1.38.0/include
)

target_compile_definitions(http_file_server PRIVATE
    ASIO_STANDALONE
)

if(SDKTARGETSYSROOT)
    target_include_directories(http_file_server PRIVATE
        ${SDKTARGETSYSROOT}/usr/include
    )
    target_link_directories(http_file_server PRIVATE
        ${SDKTARGETSYSROOT}/usr/lib
    )
endif()

# 링크 라이브러리
target_link_libraries(http_file_server PRIVATE
    Threads::Threads
)

# 디버그 빌드 시 DEBUG 매크로 추가
target_compile_definitions(http_file_server PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
)
