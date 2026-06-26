# SimpleHttpFileServer

C++20 코루틴(Asio) 기반의 간단한 HTTP 파일 서버입니다.  
지정된 디렉토리를 HTTP로 서빙하며, **Range Request**를 지원하여 대용량 파일의 부분 다운로드 및 스트리밍이 가능합니다.

## 주요 기능

- **GET / HEAD** 요청 처리
- **Range Request** 지원 (`bytes=start-end`, `bytes=start-`, `bytes=-suffix` → 206 Partial Content)
- 디렉토리 탐색 (HTML 리스팅)
- MIME 타입 자동 감지 (확장자 기반)
- URL 인코딩/디코딩 (UTF-8 경로 지원)
- Directory traversal 방지 (경로 검증)
- UDP 브로드캐스트 서버 (서비스 디스커버리용)
- Yocto SDK 크로스 컴파일 지원

## 빌드

### 요구사항

- CMake ≥ 3.16
- C++20 지원 컴파일러 (GCC 11+, Clang 14+)
- pthread

### 네이티브 빌드

```bash
mkdir build && cd build
cmake ..
make
```

### Yocto SDK 크로스 컴파일

```bash
source /opt/poky/environment-setup-*
mkdir build && cd build
cmake ..
make
```

## 실행

```bash
./http_file_server <address> <port> <id> [root_path]
```

| 인자 | 설명 |
|------|------|
| `address` | 바인딩할 주소 (예: `0.0.0.0`) |
| `port` | HTTP 리스닝 포트 (예: `8080`) |
| `id` | UDP 디스커버리용 서버 식별자 |
| `root_path` | (선택) 서빙할 루트 디렉토리. 생략 시 현재 작업 디렉토리 |

### 예시

```bash
# 현재 디렉토리를 8080 포트로 서빙
./http_file_server 0.0.0.0 8080 myserver

# /home/user/files 디렉토리를 서빙
./http_file_server 0.0.0.0 8080 myserver /home/user/files
```

## Range Request 예시

```bash
# 처음 1024 바이트만 요청
curl -H "Range: bytes=0-1023" http://localhost:8080/largefile.bin

# 1MB 오프셋부터 끝까지
curl -H "Range: bytes=1048576-" http://localhost:8080/largefile.bin

# 마지막 500 바이트
curl -H "Range: bytes=-500" http://localhost:8080/largefile.bin
```

## 프로젝트 구조

```
├── CMakeLists.txt
├── include/
│   ├── http_file_server.h      # TCP acceptor (연결 수락)
│   ├── http_session.h          # HTTP 세션 처리
│   └── udp_broadcast_server.h  # UDP 디스커버리 서버
├── src/
│   ├── main.cpp
│   ├── http_file_server.cpp
│   ├── http_session.cpp
│   └── udp_broadcast_server.cpp
└── third_party/
    └── asio_1.38.0/            # Standalone Asio (헤더 온리)
```

## 의존성

- [Asio 1.38.0](https://think-async.com/Asio/) (standalone, 동봉됨)

## 라이선스

Asio는 Boost Software License 1.0 하에 배포됩니다. (`third_party/asio_1.38.0/LICENSE_1_0.txt` 참조)
