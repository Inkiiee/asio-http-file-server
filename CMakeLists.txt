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
set(CMAKE_BUILD_TYPE Release)

project(http_file_server C CXX)

set(SRCS
    src/main.cpp
    src/http_base.cpp
    src/http_file_server.cpp
    src/http_file_system.cpp
    src/http_session.cpp
    src/http_util.cpp
    src/my_proto.cpp
    src/web_dav_proto.cpp
    third_party/pugixml/pugixml.cpp
)

add_executable(http_file_server ${SRCS})

target_compile_options(http_file_server PRIVATE
    -w
    $<$<CONFIG:Release>:-O2>

)

# 인클루드 경로
target_include_directories(http_file_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
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

# 디버그 빌드 시 DEBUG 매크로 추가
target_compile_definitions(http_file_server PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
)
