# Claude 인수인계 문서
# 다음 대화 시작 시 이 파일을 첨부할 것
#
# 파일명 규칙: CLAUDE_HANDOFF_YYYYMMDD.md
# 매 세션 종료 시 날짜를 붙여서 새로 생성할 것
# 이전 파일은 보관하고 삭제하지 말 것

## 프로젝트 개요
- Framework: ESP-IDF v6.0.1
- Device: Waveshare ESP32-S3-Touch-LCD-1.47
- LCD: JD9853 (172×320, 80MHz), Touch: AXS5106L
- PSRAM: 8MB, Flash: 16MB
- LVGL: 9.4.0, esp_lvgl_port: 2.7.0

## 현재 진행 상태 (2026-06-15)
- BSP + UI 완성 (목록/체크/타이머/폰트테스트 탭)
- 한글 폰트 (NanumGothic TTF) TinyTTF로 LittleFS에서 로드
- UI 정상 동작
- **미해결: TinyTTF kerning 버그로 특정 폰트 크기에서 자간 이상**

---

## ✅ 잘 된 것 — 계속 유지

### 폰트 구조 (TinyTTF)
- TTF 파일을 LittleFS에 저장, POSIX 드라이버(F:)로 접근
- `lv_tiny_ttf_create_data()` 사용 — TTF를 PSRAM에 올린 후 생성
  - `lv_tiny_ttf_create_file()`은 같은 파일을 여러 크기로 열면 파일 포인터 충돌 문제
- 폰트 크기: 10, 13, 14, 15, 18, 22, 30pt 미리 생성 후 static 보관
- `ui_font_get(size)` API로 접근, `ui_common.h`에 매크로 정의

### 폰트 매크로 (ui_common.h)
```c
#define UI_FONT_10  (ui_font_get(10))
#define UI_FONT_13  (ui_font_get(13))
#define UI_FONT_15  (ui_font_get(15))
#define UI_FONT_18  (ui_font_get(18))
#define UI_FONT_22  (ui_font_get(22))
#define UI_FONT_30  (ui_font_get(30))
```
- UI 전체 기본 폰트: 13pt (UI_FONT_13)
- 버튼(시작/정지): 22pt (UI_FONT_22)

### 파일 시스템
- LittleFS 파티션: 0x810000, 7.94MB
- TTF 파일: `/fonts/NanumGothic-Regular.ttf`
- POSIX 드라이버 letter: F
- VFS 경로: `/fonts/NanumGothic-Regular.ttf`
- LVGL 경로: `F:/fonts/NanumGothic-Regular.ttf`

### 스크립트 (C:\Projects\)
- `gen_littlefs.bat` → `gen_littlefs.py` 실행
- `flash_fonts.bat` → fonts.bin 플래시 (0x810000)
- 폰트 업데이트: gen_littlefs → flash_fonts

### 메모리 구조
- LVGL PSRAM 풀: 5MB (`lv_mem_add_pool`)
- TTF 버퍼: PSRAM (`heap_caps_malloc(SPIRAM)`)
- 폰트 draw buf: PSRAM (`lv_draw_buf_get_font_handlers()` 커스텀)

### 개발 환경
- 파일 교체 후 타임스탬프 갱신 필수:
  `(Get-Item startup\main\파일명).LastWriteTime = Get-Date`
- erase_flash 시 fonts 파티션도 지워짐 → flash_fonts 재실행 필요
- PowerShell ps1 서명 문제 → bat으로 python 호출
- bat에서 쉼표 포함 변수는 `--range` 두 번으로 분리

---

## ❌ 잘못된 것 — 절대 하지 말 것

### 폰트
- `lv_tiny_ttf_create_file()` 여러 크기로 동시 사용 금지
  → 파일 포인터 공유로 자간 버그 발생
- `UI_FONT_14` 매크로 사용 금지 — TinyTTF 9.4.0에서 14pt 버그
- montserrat 폰트 직접 참조 금지 (`&lv_font_montserrat_XX`)
  → 한글 없음, 반드시 `ui_font_get()` 사용

### 빌드
- 파일 교체 후 타임스탬프 갱신 없이 빌드 → 재컴파일 안 됨
- managed_components 수정 금지 → 빌드 시 덮어써짐
- erase_flash 후 fonts 파티션 재플래시 필요

---

## 🐛 미해결 버그: TinyTTF 자간 버그

### 현상
- 가장 많이 사용된 폰트 크기의 자간이 비정상 (너무 좁거나 넓어짐)
- 특정 글자 조합에서 겹치거나 간격이 이상해짐

### 원인 (확인됨)
- LVGL GitHub Issue #9429: `fix(tiny_ttf): release entry for correct cache`
- kerning entry를 `kerning_cache` 대신 `glyph_cache`로 잘못 release → 캐시 오염
- esp_lvgl_port 2.7.0으로 업그레이드했으나 여전히 동일 현상

### 시도한 것들 (모두 실패)
- 캐시 크기 증가 (256→512)
- 캐시 없애기 (cache_size=0)
- `lv_tiny_ttf_create_file()` → `lv_tiny_ttf_create_data()` 전환
- LVGL 풀 증가 (3MB→5MB)
- 폰트 draw buf PSRAM 전환
- `LV_FONT_KERNING_NONE` (폰트 생성 시)
- esp_lvgl_port 2.7.0 버전업

### 내일 시도할 것
1. 각 위젯 폰트 설정 직전 `lv_tiny_ttf_set_size()` 호출
   ```c
   lv_tiny_ttf_set_size(s_font_13, 13);
   lv_obj_set_style_text_font(label, ui_font_get(13), 0);
   ```
2. `cache_size` 1024로 증가
3. 위젯 레벨 kerning 비활성화:
   ```c
   lv_obj_set_style_text_kerning(label, LV_FONT_KERNING_NONE, 0);
   ```

---

## 파티션 구조
```
nvs       0x9000    24KB
phy_init  0xF000     4KB
app       0x10000    8MB
fonts     0x810000   7.94MB (LittleFS)
```

## 핵심 sdkconfig 설정
```
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_LV_USE_TINY_TTF=y
CONFIG_LV_TINY_TTF_FILE_SUPPORT=y
CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT=512
CONFIG_LV_USE_FS_POSIX=y
CONFIG_LV_FS_POSIX_LETTER=70
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

## 이후 작업
1. 폰트 테스트 탭 정리
2. 색상 팔레트 이름 한글화
3. git 커밋
4. Sub Node (ESP32-C): ESP-Now, Deep Sleep, 배터리
5. Main Gateway: WiFi/MQTT/ThingsBoard CE
