# asio-http-file-server

C++20과 standalone Asio 기반의 간단한 HTTP 파일 서버입니다. 지정한 루트 디렉터리 안의 파일을 다운로드/업로드하고, JSON API 또는 WebDAV 메서드로 파일과 디렉터리를 조작할 수 있습니다.

## 주요 기능

- 파일 다운로드: `GET`
- 파일 업로드/덮어쓰기: `PUT`
- 디렉터리 목록 조회: `GET` 요청 시 JSON 배열 반환
- Range Request 지원: `bytes=start-end`, `bytes=start-`, `bytes=-suffix`
- 파일 MIME 타입 자동 판별
- UTF-8 URL 디코딩/인코딩
- 루트 디렉터리 밖 접근 차단
- JSON 기반 파일 조작 API: copy, move, delete, mkdir
- WebDAV 일부 지원: `OPTIONS`, `PROPFIND`, `PROPPATCH`, `MKCOL`, `COPY`, `MOVE`, `DELETE`, `LOCK`, `UNLOCK`
- ETag, Last-Modified, 조건부 `PUT` 헤더 일부 지원

## 의존성

- CMake 3.16 이상
- C++20 지원 컴파일러
- pthread
- 포함된 third-party 라이브러리
  - standalone Asio 1.38.0
  - nlohmann/json
  - pugixml

## 빌드

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Yocto SDK 환경에서는 SDK 환경 스크립트를 먼저 로드한 뒤 동일하게 빌드합니다.

```bash
source /opt/poky/environment-setup-*
mkdir -p build
cd build
cmake ..
cmake --build .
```

## 실행

```bash
./http_file_server <address> <port> [root_path]
```

| 인자 | 설명 |
| --- | --- |
| `address` | 바인딩할 주소. 예: `0.0.0.0`, `127.0.0.1` |
| `port` | HTTP 포트. 예: `8080` |
| `root_path` | 선택값. 서비스할 루트 디렉터리. 생략하면 현재 작업 디렉터리를 사용합니다. |

예시:

```bash
./http_file_server 0.0.0.0 8080
./http_file_server 0.0.0.0 8080 /home/user/files
```

## 기본 HTTP 사용법

파일 다운로드:

```bash
curl -O http://localhost:8080/example.txt
```

디렉터리 목록 조회:

```bash
curl http://localhost:8080/
```

응답은 다음 형태의 JSON 배열입니다.

```json
[
  {
    "name": "docs",
    "path": "/docs/",
    "is_directory": true,
    "size": 0,
    "extension": "",
    "last_modified": "Wed, 08 Jul 2026 02:30:00 GMT"
  }
]
```

파일 업로드:

```bash
curl -T local.txt http://localhost:8080/upload/local.txt
```

새 파일일 때만 업로드:

```bash
curl -T local.txt -H "If-None-Match: *" http://localhost:8080/upload/local.txt
```

## Range Request

```bash
curl -H "Range: bytes=0-1023" http://localhost:8080/largefile.bin -o part.bin
curl -H "Range: bytes=1048576-" http://localhost:8080/largefile.bin -o tail.bin
curl -H "Range: bytes=-500" http://localhost:8080/largefile.bin -o last500.bin
```

정상 범위 요청은 `206 Partial Content`와 `Content-Range`를 반환합니다.

## JSON 파일 조작 API

모든 요청은 `POST`와 `Content-Type: application/json`을 사용합니다. 경로는 서버 루트 기준 경로입니다.

### 파일/디렉터리 복사

```bash
curl -X POST http://localhost:8080/file/copy \
  -H "Content-Type: application/json" \
  -d '{"src":"/docs/a.txt","dst":"/backup/a.txt"}'
```

### 파일/디렉터리 이동

```bash
curl -X POST http://localhost:8080/file/move \
  -H "Content-Type: application/json" \
  -d '{"src":"/docs/a.txt","dst":"/docs/b.txt"}'
```

### 파일/디렉터리 삭제

```bash
curl -X POST http://localhost:8080/file/delete \
  -H "Content-Type: application/json" \
  -d '{"file_path":"/docs/b.txt"}'
```

### 디렉터리 생성

```bash
curl -X POST http://localhost:8080/directory/create \
  -H "Content-Type: application/json" \
  -d '{"dir_path":"/docs/new/sub"}'
```

성공 시 JSON 응답은 보통 다음 형태입니다.

```json
{"success":"OK"}
```

실패 시에는 상태 코드와 함께 `error` 필드가 반환됩니다.

```json
{"error":"Not Found"}
```

## WebDAV 사용법

지원 메서드는 다음과 같습니다.

| 메서드 | 동작 |
| --- | --- |
| `OPTIONS` | 지원 메서드와 DAV 헤더 반환 |
| `PROPFIND` | 파일/디렉터리 속성 조회 |
| `PROPPATCH` | 요청된 속성에 대해 `207 Multi-Status` 응답 |
| `MKCOL` | 디렉터리 생성 |
| `COPY` | 파일/디렉터리 복사 |
| `MOVE` | 파일/디렉터리 이동 |
| `DELETE` | 파일/디렉터리 삭제 |
| `LOCK` | 단순 lock token 응답 |
| `UNLOCK` | `204 No Content` 응답 |

PROPFIND 예시:

```bash
curl -X PROPFIND http://localhost:8080/docs/ -H "Depth: 1"
```

MKCOL 예시:

```bash
curl -X MKCOL http://localhost:8080/docs/newdir
```

COPY 예시:

```bash
curl -X COPY http://localhost:8080/docs/a.txt \
  -H "Destination: http://localhost:8080/docs/a-copy.txt" \
  -H "Overwrite: T"
```

MOVE 예시:

```bash
curl -X MOVE http://localhost:8080/docs/a-copy.txt \
  -H "Destination: http://localhost:8080/docs/a-moved.txt" \
  -H "Overwrite: T"
```

DELETE 예시:

```bash
curl -X DELETE http://localhost:8080/docs/a-moved.txt
```

## 프로젝트 구조

```text
.
|-- CMakeLists.txt
|-- include/
|   |-- base_proto.h
|   |-- http_base.h
|   |-- http_file_entry.h
|   |-- http_file_server.h
|   |-- http_file_system.h
|   |-- http_session.h
|   |-- http_util.h
|   |-- my_proto.h
|   `-- web_dav_proto.h
|-- src/
|   |-- main.cpp
|   |-- http_base.cpp
|   |-- http_file_server.cpp
|   |-- http_file_system.cpp
|   |-- http_session.cpp
|   |-- http_util.cpp
|   |-- my_proto.cpp
|   `-- web_dav_proto.cpp
|-- third_party/
|   |-- asio_1.38.0/
|   |-- nlohmann/
|   `-- pugixml/
`-- licenses/
```

## 라이선스

third-party 라이브러리의 라이선스는 `licenses/`와 각 라이브러리 디렉터리의 라이선스 파일을 참고하세요.
