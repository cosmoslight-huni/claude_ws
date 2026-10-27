# BMS Agent PC 시뮬레이터

BMS 에이전트의 **Web 설정(IP/NTP/SNMP/알람) + SNMP Agent + NTP 클라이언트**를 PC에서 먼저 개발·검증하기 위한 시뮬레이터입니다.
`src/core`는 플랫폼 독립 C++17 코드로, 나중에 ESP32 / Arduino Opta로 그대로 옮길 수 있도록 하드웨어 접근을 모두 인터페이스(`hal.h`)로 분리했습니다.

```
 ┌────────────── src/core (보드에 그대로 이식) ──────────────┐
 │ App ─ ConfigStore(JSON) ─ AlarmEngine ─ NtpClient          │
 │  │                                                         │
 │  ├─ WebApi (REST)          snmp::Agent + BmsMib (v1/v2c)   │
 └──┼───────────────────────────────┼─────────────────────────┘
    │ hal.h: IStorage / IUdp / INetworkIf / IBmsSource / ISystem
 ┌──┴──────────── src/pc (PC 전용) ─────────────────────────┐
 │ HttpServer · PosixUdp · FileStorage · SimNetwork · SimBms │
 └───────────────────────────────────────────────────────────┘
```

## 빌드 / 실행

필요: CMake 3.13+, g++ 또는 clang (C++17). Linux / macOS / WSL에서 동작합니다. Windows는 WSL을 사용하세요.

```bash
cd bms_agent_sim
cmake -S . -B build && cmake --build build -j
./build/core_tests                      # 단위 테스트
./build/bms_agent_sim --snmp-port 1161  # 실행
```

브라우저에서 http://localhost:8080 에 접속합니다. 초기 계정은 `admin / admin`입니다.

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--http-port` | 8080 | Web UI/REST 포트 |
| `--snmp-port` | (설정값 161) | SNMP 포트 강제 지정. 일반 사용자는 161을 열 수 없으므로 1161 등을 사용 |
| `--data-dir` | `./data` | 설정 파일(`config.json`, `config.bak`) 저장 위치 |
| `--web-dir` | `./web` | Web UI 정적 파일 위치 |
| `--rollback-sec` | 120 | 네트워크 변경 후 자동 롤백까지의 시간 |

## 기능

### Web UI / REST API
모든 API는 `/api/login`에서 받은 토큰이 필요합니다(`Authorization: Bearer <token>`).

| Method | URL | 기능 |
|---|---|---|
| POST | `/api/login`, `/api/logout` | 로그인(5회 실패 시 30초 잠금), 로그아웃 |
| GET | `/api/status` | 팩/셀 실시간 값, 시각, 알람 개수 |
| GET | `/api/alarms` | 활성 알람 + 이력(최근 50건) |
| GET/PUT | `/api/config/network` | IP 설정 (DHCP/고정, 마스크·게이트웨이 검증) |
| POST | `/api/config/network/confirm` | 변경 확정 (안 하면 `--rollback-sec` 후 자동 복구) |
| GET/PUT | `/api/config/ntp` | NTP 서버 2개, 주기, 타임존 |
| GET/PUT | `/api/system/time` | 현재 시각 조회 / 수동 설정(폐쇄망용) |
| POST | `/api/system/time-sync` | 즉시 NTP 동기화 |
| GET/PUT | `/api/config/snmp` | 커뮤니티, sys 정보, Trap 대상(최대 4개), 통계 |
| GET/PUT | `/api/config/alarm`, `/api/config/bms` | 알람 임계값, 폴링 주기/셀 개수 |
| PUT | `/api/config/password` | 비밀번호 변경 (salt + SHA-256 저장) |
| GET / POST | `/api/config/backup`, `/api/config/restore` | 설정 백업/복원 |
| POST | `/api/system/reboot`, `/api/system/factory-reset` | 재시작(시뮬레이션), 공장 초기화 |
| GET/PUT | `/api/sim` | **PC 전용** 시뮬레이터 제어·고장 주입 |

### SNMP Agent
- SNMP v1/v2c를 지원합니다: GET, GETNEXT, GETBULK, SET, v2c Trap.
- MIB-II system 그룹과 사설 MIB(`mib/BMS-SIM-MIB.txt`, smilint 검증 완료)를 제공합니다.
- 기업 번호 `99999`는 **임시 값**입니다. 실제 배포 전에 IANA에서 PEN을 발급받아 `mib/BMS-SIM-MIB.txt`와 `src/core/bms_mib.h`를 함께 바꾸세요.

```
1.3.6.1.4.1.99999.1 bmsAgent
├── .0  bmsNotifications   .1 bmsAlarmRaised  .2 bmsAlarmCleared
├── .1  bmsSystem          모델, 펌웨어, 현재 시각, NTP 동기화 여부
├── .2  bmsStatus          팩 전압(mV)/전류(mA)/SOC/SOH/최대·최소 셀/최고 온도/상태/알람 수/통신/셀 수
├── .3  bmsCellTable       셀별 전압(mV), 온도(0.1°C)
├── .4  bmsAlarmTable      활성 알람 (인덱스 = 알람 코드)
└── .5  bmsConfig          알람 임계값 (RW 커뮤니티로 SET 가능)
```

```bash
snmpwalk -v2c -c public localhost:1161 1.3.6.1.4.1.99999
snmpset  -v2c -c private localhost:1161 1.3.6.1.4.1.99999.1.5.4.0 i 25   # SOC 하한 25%
snmpwalk -v2c -c public -M +./mib -m +BMS-SIM-MIB localhost:1161 bmsAgent  # MIB 이름으로 보기 (표준 MIB 필요: apt install snmp-mibs-downloader)
```

### 시뮬레이터 (가상 배터리)
- 16셀 LFP, 100Ah 팩을 흉내 냅니다. 기본은 60배속 자동 충방전입니다(SOC 30% ↔ 95%).
- 시뮬레이터 탭이나 `/api/sim`에서 고장을 주입할 수 있습니다.
  - `{"cell":3,"mv_offset":500}`: 셀 3 과전압
  - `{"cell":5,"temp_offset_dc":350}`: 셀 5 온도 +35°C
  - `{"comm_fail":true}`: BMS 통신 끊김
  - `{"soc":15}`: SOC 강제 설정
  - `{"reset_faults":true}`: 모든 고장 해제

## 테스트 도구

| 파일 | 용도 |
|---|---|
| `tests/core_tests.cpp` | 코어 단위 테스트 (BER 인코딩, SNMP 처리, 설정 검증, 알람 히스테리시스 등) |
| `tests/smoke_test.sh` | 전체 통합 테스트: Web API, IP 롤백, SNMP get/walk/bulk/set, Trap, NTP, 재부팅 후 설정 유지 |
| `tests/trap_listener.py` | Trap 수신 확인 (`python3 tests/trap_listener.py --port 1162`) |
| `tests/fake_ntp_server.py` | 인터넷 없이 NTP 테스트 (`sudo python3 tests/fake_ntp_server.py`, NTP 서버를 127.0.0.1로) |

`smoke_test.sh`는 net-snmp 명령(`snmpget` 등)이 필요합니다. Ubuntu에서는 `sudo apt install snmp`로 설치합니다.

## ESP32 / Opta로 옮길 때

`src/core`는 그대로 두고 `src/pc`에 해당하는 부분만 보드용으로 새로 작성하면 됩니다.

| 인터페이스 | ESP32 (Arduino) | Arduino Opta |
|---|---|---|
| `IStorage` | LittleFS (`rename`으로 원자적 저장) | KVStore 또는 LittleFS |
| `IUdp` | `WiFiUDP` / `NetworkUDP` | `EthernetUDP` |
| `INetworkIf` | `ETH.config(ip, gw, mask, dns)` | `Ethernet.begin(ip, dns, gw, mask)` |
| `IBmsSource` | RS485 Modbus 드라이버 (ModbusMaster 등) | 내장 RS485 + ArduinoModbus |
| `ISystem` | `millis()`, `esp_random()`, `ESP.restart()` | `millis()`, `NVIC_SystemReset()` |
| HTTP 서버 | ESPAsyncWebServer → `WebApi::handle()` 호출 | `EthernetServer` 기반 간단 서버 |
| Web 파일 | `web/index.html`을 gzip으로 압축해 LittleFS에 업로드 | 같은 방식 |

- JSON은 PC에서도 **ArduinoJson**(`third_party/ArduinoJson.h`, v7.2.1, MIT)을 써서 보드와 코드가 같습니다.
- 메인 루프는 Arduino의 `loop()`와 같은 단일 스레드 구조입니다(`App::tick()` 반복 호출). RTOS 태스크로 나눌 필요가 없습니다.
- RTC가 있는 보드(Opta)는 부팅 시 RTC 값으로 `Clock`을 초기화하면 됩니다.

## 알려진 제한
- SNMPv3, HTTPS는 아직 없습니다. ESP32에서는 `esp_https_server`를 쓰고, SNMPv3는 필요할 때 추가합니다.
- PC의 네트워크 설정은 **흉내만** 냅니다. 실제 NIC는 바꾸지 않고 로그로만 보여줍니다.
- 네트워크 롤백 대기 중에 재부팅하면 새 설정이 그대로 유지됩니다. 실제 장비에서는 대기 상태를 저장하거나 공장 초기화 버튼으로 대비해야 합니다.
