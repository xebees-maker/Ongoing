#include "mq137.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "mq137";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_channel_t             s_ao_channel  = 0;
static gpio_num_t                s_do_gpio     = GPIO_NUM_NC;

/* 2026-09-12(사용자 지시 — "변환해", 값들은 mq137.h 파일 설명 참고, 전부 잠정치)
 *
 * 2026-09-12 추가 조사 결과(웹 검색, 사용자 지시 "웹에서 아티클도 좀 찾아보고. 초기 설정 Vr
 * 값도 찾아보고"):
 *   - 이 보드류(LM393 비교기 탑재 일반 MQ 브레이크아웃)는 보통 트리머(Vr)가 RL이 아니라
 *     DO 코앞의 비교기 기준전압(임계값)만 조정하고, AO는 보드 내 고정 저항을 통해 나온다는
 *     사례가 다수 — "RL=10kΩ은 트리머 기본값"이라는 아래 가정은 확인되지 않음(단, Rs/Ro
 *     비율 계산에서는 RL 오차가 있어도 Ro와 Rs 양쪽에 동일 배율로 걸려 상쇄되므로 ppm 비율
 *     자체는 영향 없음 — 그러나 Rs 절대값(Ω) 표시 용도로는 부정확함, TODO).
 *   - Winsen 공식 매뉴얼(ver1.4, 이번에 원문 재확인)의 Fig5 그래프는 RL=4.7KΩ 기준(트리머
 *     아님, 고정 테스트 조건)이라고 명시 — 이전 "10kΩ 트리머 기본값" 주석은 근거 불명확.
 *   - a=102.6, b=-2.48 계수의 출처도 재확인 필요: Winsen Fig3 그래프상 Ro 정의(맑은 공기
 *     저항)라면 Rs/Ro=1일 때 ppm은 0에 가까워야 하는데, 이 공식은 ratio=1일 때 정확히
 *     a(=102.6)를 반환함 — 즉 이 a,b 값 자체가 실제 그래프를 정확히 피팅한 것인지 의심스러움
 *     (사용자도 "피팅은 나중에 추가로 해야겠지" 라고 명시적으로 잠정치임을 인정한 바 있음).
 *
 * 위 문제로 2026-09-12 "71ppm 나오는데 0에 수렴하도록 보정" 지시에 대응해 아래처럼 변경:
 * 순수 거듭제곱식은 어떤 a,b를 쓰든 ratio=1(Rs=Ro, 기준 상태)에서 정확히 0을 낼 수 없는
 * 구조라서(0으로 수렴하려면 ratio->무한대), a,b를 건드리는 대신 "Ro 기준점 대비 초과분"으로
 * 재정의: raw = a×ratio^b (ratio=1일 때 raw=a), 최종 ppm = max(0, raw - a).
 * 이러면 기준점(Ro 측정 당시의 맑은 공기)과 같거나 더 깨끗한 상태는 전부 0으로, 기준보다
 * 저항이 낮아지는(가스 농도가 오르는) 쪽으로만 0 초과값이 나옴. */
#define MQ137_VC_MV        5000.0f   /* Winsen 데이터시트: Loop Voltage Vc = 5.0V±0.1V DC */
#define MQ137_RL_OHM       10000.0f  /* 근거 불명확(TODO) — 위 주석 참고, ppm 비율 계산에는
                                        영향 없음(Ro/Rs 양쪽에 동일 배율로 상쇄) */
#define MQ137_RO_OHM       16860.0f  /* 고정값 — 2026-09-12 맑은 공기 확인 상태의 AO=1861.5mV
                                        1회 측정치로부터 역산(TODO: 나중에 실제 보정 필요) */
#define MQ137_CURVE_A      102.6f    /* TODO: 나중에 실제 보정 필요, 출처 신뢰도 낮음(위 주석) */
#define MQ137_CURVE_B      (-2.48f)  /* TODO: 나중에 실제 보정 필요 */

bool mq137_init(adc_oneshot_unit_handle_t adc_handle, adc_channel_t ao_channel,
                 adc_atten_t atten, gpio_num_t do_gpio)
{
    if (!adc_handle) return false;

    adc_oneshot_chan_cfg_t ao_cfg = {
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_handle, ao_channel, &ao_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "AO 채널 설정 실패");
        return false;
    }

    gpio_config_t do_cfg = {
        .pin_bit_mask = 1ULL << do_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&do_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "DO 핀 설정 실패");
        return false;
    }

    s_adc_handle = adc_handle;
    s_ao_channel = ao_channel;
    s_do_gpio    = do_gpio;
    return true;
}

bool mq137_read(float *ppm, bool *do_alarm)
{
    if (!s_adc_handle) return false;

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, s_ao_channel, &raw) != ESP_OK) return false;

    /* 2026-09-12 — sensor_node.c의 vin_indicates_usb()와 동일한 raw->mV 환산식
     * (12bit, ADC_ATTEN_DB_12 기준 대략치) 재사용 — 정밀 보정은 나중 과제 */
    float ao_mv = (float)raw * 3100.0f / 4095.0f;

    if (ppm) {
        /* Winsen 공식 저항식: Rs = (Vc/VRL - 1) × RL (AO=VRL 가정) */
        float vrl_mv = (ao_mv < 1.0f) ? 1.0f : ao_mv;  /* 0으로 나누기 방지 */
        float rs_ohm = (MQ137_VC_MV / vrl_mv - 1.0f) * MQ137_RL_OHM;
        float ratio  = rs_ohm / MQ137_RO_OHM;
        float raw_ppm = MQ137_CURVE_A * powf(ratio, MQ137_CURVE_B);
        /* ratio=1(Rs=Ro, 기준 맑은 공기)일 때 raw_ppm은 항상 MQ137_CURVE_A와 같음 — 이를
         * 빼서 "Ro 기준 대비 초과분"으로 재정의하고 음수는 0으로 클램프 */
        float ppm_above_baseline = raw_ppm - MQ137_CURVE_A;
        *ppm = (ppm_above_baseline < 0.0f) ? 0.0f : ppm_above_baseline;
    }
    if (do_alarm) *do_alarm = (gpio_get_level(s_do_gpio) == 0);

    return true;
}
