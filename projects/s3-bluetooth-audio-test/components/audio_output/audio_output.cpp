// s3-bluetooth-audio/components/audio_output/audio_output.cpp
#include "audio_output.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "AUDIO_OUTPUT";

AudioOutput::AudioOutput() {}

AudioOutput::~AudioOutput() {
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    ready_ = false;
}

bool AudioOutput::begin() {
    if (ready_) return true;

    // 配置 I2S GPIO 引脚为输出模式
    gpio_config_t gpio_conf = {};
    gpio_conf.pin_bit_mask =
        (1ULL << I2S_BCK_PIN) |
        (1ULL << I2S_WS_PIN)  |
        (1ULL << I2S_SDOUT_PIN);
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&gpio_conf);

    // 创建 I2S TX 通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // DMA 缓冲区耗尽后自动填充 0

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 通道创建失败: %s", esp_err_to_name(err));
        return false;
    }

    // 配置 I2S 标准模式（Philips/WS-based I2S）
    i2s_std_config_t std_cfg = {};
    // ESP32-S3: 使用 PLL_160M 作为 I2S 时钟源（APLL 不可用）
    std_cfg.clk_cfg.sample_rate_hz = AUDIO_SAMPLE_RATE;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    std_cfg.clk_cfg.mclk_div = 0;                // 驱动自动计算

    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;           // MSB 在前
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT | I2S_STD_SLOT_RIGHT;

    std_cfg.gpio_cfg.bclk_io_num = I2S_BCK_PIN;
    std_cfg.gpio_cfg.ws_io_num = I2S_WS_PIN;
    std_cfg.gpio_cfg.dout_io_num = I2S_SDOUT_PIN;
    std_cfg.gpio_cfg.mclk_io_num = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;

    err = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 标准模式初始化失败: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    err = i2s_channel_enable(tx_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 通道使能失败: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    ready_ = true;
    ESP_LOGI(TAG, "I2S 音频输出就绪 (BCK=GPIO%u, WS=GPIO%u, DOUT=GPIO%u)",
             I2S_BCK_PIN, I2S_WS_PIN, I2S_SDOUT_PIN);
    return true;
}

size_t AudioOutput::write(const uint8_t* data, size_t len) {
    if (!ready_ || tx_handle_ == nullptr || data == nullptr || len == 0) {
        return 0;
    }
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, data, len, &bytes_written, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(err));
        return 0;
    }
    return bytes_written;
}

size_t AudioOutput::writeSilence(size_t len) {
    if (!ready_ || len == 0) return 0;
    static uint8_t silence[AUDIO_FRAME_BYTES] = {0};
    size_t total = 0;
    const size_t maxRetries = 10;
    size_t retries = 0;

    while (total < len && retries < maxRetries) {
        size_t chunk = (len - total) > AUDIO_FRAME_BYTES ? AUDIO_FRAME_BYTES : (len - total);
        ssize_t written = write(silence, chunk);
        if (written > 0) {
            total += written;
            retries = 0;  // Reset on success
        } else {
            retries++;
        }
    }
    // If we exit due to retries exhausted, that's OK - audio will have gaps
    return total;
}
