cmake_minimum_required(VERSION 3.10)
project(imgconverter CXX)

# C++17 
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# OpenCV
find_package(OpenCV REQUIRED)

# 소스 파일
set(SOURCES main.cpp)

# 실행 파일
add_executable(${PROJECT_NAME} ${SOURCES})

# OpenCV 인클루드 디렉토리 및 라이브러리 링크
target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})

# Windows 환경일 경우 psapi.lib 링크 추가
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE psapi)
endif()