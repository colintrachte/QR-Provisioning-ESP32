/**
 * i_battery.c — Battery voltage measurement via ADC1_CH0 (GPIO1).
 *
 * IDF 5.x ADC API: esp_adc_cal is deprecated in favour of adc_cali_*.
 * We use the new scheme with a oneshot ADC driver and the curve-fitting
 * calibration scheme where available (falls back gracefully if efuse
 * calibration data was not burned at the factory).
 */

#include "i_battery.h"
#include "config.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "i_battery";

static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_initialized = false;

/* ── Rolling average ────────────────────────────────────────────────────────*/
static uint32_t s_samples[I_BATTERY_SAMPLES] = {0};
static int      s_sample_idx = 0;
static bool     s_buf_full   = false;

static uint32_t rolling_average_mv(uint32_t new_mv)
{
    s_samples[s_sample_idx] = new_mv;
    s_sample_idx = (s_sample_idx + 1) % I_BATTERY_SAMPLES;
    if (s_sample_idx == 0) s_buf_full = true;

    int count = s_buf_full ? I_BATTERY_SAMPLES : s_sample_idx;
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) sum += s_samples[i];
    return sum / count;
}

/* ── Init ───────────────────────────────────────────────────────────────────*/

void i_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   /* 0–3.3 V range (3.9 V with S3 OTP) */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return;
    }

    /* Attempt curve-fitting calibration (requires burned eFuse data) */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .chan     = BATTERY_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration not available — readings will be uncalibrated");
        s_cali_handle = NULL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC init OK (GPIO%d, cali=%s)",
             1, s_cali_handle ? "yes" : "no");
}

/* ── Read ───────────────────────────────────────────────────────────────────*/

uint32_t i_battery_read_mv(void)
{
    if (!s_initialized) return 0;

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw) != ESP_OK)
        return 0;

    int vadc_mv = 0;
    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &vadc_mv);
    } else {
        /* Uncalibrated: linear approximation for 12 dB atten, 12-bit */
        vadc_mv = (raw * 3300) / 4095;
    }

    /* Scale by divider ratio: VBAT = VADC × (R_TOP + R_BOT) / R_BOT */
    uint32_t vbat_mv = (uint32_t)vadc_mv *
                       (BATTERY_R_TOP + BATTERY_R_BOT) / BATTERY_R_BOT;

    return rolling_average_mv(vbat_mv);
}

uint8_t i_battery_percent(void)
{
    uint32_t mv = i_battery_read_mv();
    if (mv <= BATTERY_V_EMPTY_MV) return 0;
    if (mv >= BATTERY_V_FULL_MV)  return 100;

    return (uint8_t)(100UL * (mv - BATTERY_V_EMPTY_MV) /
                             (BATTERY_V_FULL_MV - BATTERY_V_EMPTY_MV));
}
