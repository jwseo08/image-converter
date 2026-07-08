# JPG to PNG Image Converter

이 프로젝트는 OpenCV를 활용하여 JPG 이미지를 PNG로 변환하는 C++ 기반의 커맨드라인 프로그램 입니다. 
대량의 이미지를 처리하는 상황에서 순차 처리와 병렬 처리의 작업 속도를 비교할 수 있도록 구성되었습니다.
병렬 처리 작업은 스레드 풀을 적용했습니다.
처리 방식에 따른 성능 차이를 비교할 수 있도록 소요 시간을 출력하며, 메모리 사용량을 기록하는 기능을 포함하고 있습니다.

## 주요 기능

* 멀티스레딩 최적화 : `std::thread`, `std::mutex`, `std::condition_variable`을 활용한 스레드 풀 구현
* 비동기 처리 및 순서 보장 : 병렬로 이미지를 인코딩하더라도 `ReorderBuf` 클래스를 통해 원본 입력 순서와 동일하게 출력 파일을 저장
* 크로스 플랫폼 : Windows와 Linux 환경에서 사용 가능
* 사용자 옵션 제공 : 순차 처리와 병렬 처리 선택, 반복 횟수, 입출력 디렉토리 지정

## 기술 스택 (Tech Stack)

* language : C++17
* library : OpenCV 4.x
* build : CMake

## 빌드 및 실행 방법

이 프로젝트는 Ubuntu 22.04와 Windows 11에서 테스트되었습니다.
이 프로젝트는 CMake를 사용하여 빌드합니다.

### Linux
* mkdir build
* cd build
* cmake ..
* make

### Windows
* Visual Studio 2022 사용

## 성능 평가

대량의 이미지 변환 시 병렬 처리(Thread Pool) 도입에 따른 성능 향상 측정

### 테스트 환경
* CPU : Intel Core i9-13900K (3.00 GHz)
* Dataset : 640 x 480 해상도의 JPG 이미지 500장

### 처리 소요 시간
* 순차 처리 : 9777.35 ms
* 병렬 처리 : 2426.36 ms 

### 결과
* 병렬 처리 시 순차 처리 대비 약 4.02배 성능 향상
