/*
 * ESP32-S3 USB Audio Device
 * 完整的 USB Audio Class 2.0 实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOSConfig.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "sdkconfig.h"

#ifdef CONFIG_TINYUSB_ENABLED
#include "tinyusb.h"
#include "tusb_audio.h"
#include "class/audio/audio.h"
#endif

static const char *TAG = "USB_AUDIO";

// Audio configuration
#define AUDIO_SAMPLE_RATE     44100
#define AUDIO_CHANNELS       2
#define AUDIO_BIT_DEPTH      16
#define AUDIO_BYTES_PER_SAMPLE (AUDIO_BIT_DEPTH / 8)
#define AUDIO_FRAME_SIZE     (AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)

// I2S pins
#define I2S_BCK_PIN          7
#define I2S_WS_PIN           5
#define I2S_DATA_IN_PIN      6

// Buffers
#define I2S_BUFFER_SIZE      512
static uint8_t i2s_rx_buffer[I2S_BUFFER_SIZE];

// EP sizes (Full Speed max = 1023 bytes)
#define EP_IN_SIZE           256
#define EP_OUT_SIZE          256

// Software buffer sizes (must be >= EP size)
#define SW_BUFFER_SIZE       512

#ifdef CONFIG_TINYUSB_ENABLED

//====================================================================
// USB Audio Descriptor - UAC2 Speaker + Microphone
//====================================================================
// This is a minimal UAC2 descriptor for a headphone set (speaker + mic)

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_IN,    // Microphone
    ITF_NUM_AUDIO_STREAMING_OUT,    // Speaker
    ITF_NUM_CDC,                    // Serial
    ITF_NUM_TOTAL
};

// Descriptor lengths
#define TUD_AUDIO_DESC_LEN (9 + 7 + 9 + 5 + 7 + 9 + 5 + 7 + 9 + 5 + 9 + 9 + 9 + 7)
#define TUD_AUDIO_DESC_LEN2 (TUD_AUDIO_DESC_LEN + 58) // + AC interface

// Complete USB Audio 2.0 descriptor for speaker + microphone
static uint8_t const desc_audio[] = {
    // Interface Association Descriptor (IAD) for Audio
    0x09, 0x0B, ITF_NUM_AUDIO_CONTROL, 0x03, 0x01, 0x01, 0x00, 0x00,

    // Audio Control (AC) Interface - Standard AC Interface Descriptor
    0x09, 0x04, ITF_NUM_AUDIO_CONTROL, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,

    // Audio Control Interface - Class-Specific AC Interface Descriptor (Header)
    0x09, 0x24, 0x01, 0x00, 0x02, 0x29, 0x00, 0x02, 0x01,

    // Input Terminal - Microphone (ID 1)
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, AUDIO_CHANNELS, 0x00, 0x00, 0x00, 0x00,

    // Feature Unit - Microphone (ID 2)
    0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,

    // Output Terminal - USB Out (ID 3)
    0x09, 0x24, 0x03, 0x03, 0x01, 0x06, 0x00, 0x02, 0x00,

    // Input Terminal - USB In (ID 4)
    0x0C, 0x24, 0x02, 0x04, 0x01, 0x01, 0x00, AUDIO_CHANNELS, 0x00, 0x00, 0x00, 0x00,

    // Feature Unit - Speaker (ID 5)
    0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x00, 0x01, 0x00,

    // Output Terminal - Speaker (ID 6)
    0x09, 0x24, 0x03, 0x06, 0x01, 0x03, 0x00, 0x05, 0x00,

    // Audio Streaming Interface (Microphone - IN) Alt 0: Zero Bandwidth
    0x07, 0x04, ITF_NUM_AUDIO_STREAMING_IN, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    // Audio Streaming Interface (Microphone - IN) Alt 1: Operational
    0x07, 0x04, ITF_NUM_AUDIO_STREAMING_IN, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    // Audio Stream Endpoint Descriptor (Microphone IN EP)
    0x09, 0x05, 0x81, 0x05, LO_UINT16(EP_IN_SIZE), HI_UINT16(EP_IN_SIZE), 0x01, 0x00, 0x00,

    // Audio Stream Endpoint Descriptor - Class-Specific (Microphone)
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // Audio Streaming Interface (Speaker - OUT) Alt 0: Zero Bandwidth
    0x07, 0x04, ITF_NUM_AUDIO_STREAMING_OUT, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    // Audio Streaming Interface (Speaker - OUT) Alt 1: Operational
    0x07, 0x04, ITF_NUM_AUDIO_STREAMING_OUT, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    // Audio Stream Endpoint Descriptor (Speaker OUT EP)
    0x09, 0x05, 0x01, 0x05, LO_UINT16(EP_OUT_SIZE), HI_UINT16(EP_OUT_SIZE), 0x01, 0x00, 0x00,

    // Audio Stream Endpoint Descriptor - Class-Specific (Speaker)
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
};

// CDC Interface
static uint8_t const desc_cdc[] = {
    // Interface Descriptor
    0x09, 0x04, ITF_NUM_CDC, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    // CDC Header
    0x05, 0x24, 0x00, 0x10, 0x01,
    // CDC ACM
    0x04, 0x24, 0x02, 0x02,
    // CDC Union
    0x05, 0x24, 0x06, ITF_NUM_CDC, 0x00,
    // Endpoint Notification
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x08,
    // Endpoint Data
    0x07, 0x05, 0x81, 0x03, LO_UINT16(64), HI_UINT16(64), 0x00,
};

// Device descriptor
static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,  // Espressif
    .idProduct = 0x4001,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

// String descriptors
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: English
    "Espressif",                   // 1: Manufacturer
    "ESP32-S3 USB Audio",          // 2: Product
    "123456",                      // 3: Serial
};

// Configuration descriptor
static uint8_t const desc_configuration[] = {
    // Configuration
    0x09, 0x02, sizeof(desc_configuration), 0x00, 0x03, 0x01, 0x00, 0x80, 0x32,

    // Audio descriptors
    [9] = TUD_AUDIO_DESC_IAD,
    [10] = TUD_AUDIO_DESC_STD_AC,
    [19] = TUD_AUDIO_DESC_CS_AC,
    // AC header + terminals...
    [43] = TUD_AUDIO_DESC_STD_AS_IN(EP_IN_SIZE),
    [57] = TUD_AUDIO_DESC_CS_AS,
    [64] = TUD_AUDIO_DESC_STD_AS_OUT(EP_OUT_SIZE),
    [78] = TUD_AUDIO_DESC_CS_AS,
};

// Descriptor lengths array
static const uint16_t audio_desc_lengths[] = {
    TUD_AUDIO_DESC_LEN
};

//====================================================================
// TinyUSB Callbacks
//====================================================================

// Mount callback
bool tud_audio_mount_cb(uint8_t rhport, uint8_t itf, uint8_t alt_idx, uint8_t n_itf,
                       uint8_t in_ep, uint8_t out_ep) {
    ESP_LOGI(TAG, "Audio mounted! itf=%d, alt=%d, in_ep=0x%02X, out_ep=0x%02X",
             itf, alt_idx, in_ep, out_ep);
    return true;
}

// Unmount callback
void tud_audio_unmount_cb(uint8_t rhport, uint8_t itf) {
    ESP_LOGI(TAG, "Audio unmounted! itf=%d", itf);
}

// TX pre-load callback (send data to host)
void tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)itf;
    (void)ep;
    (void)cur_alt_setting;

    // Read from I2S and send to USB
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, i2s_rx_buffer, I2S_BUFFER_SIZE, &bytes_read, 5 / portTICK_PERIOD_MS);

    if (err == ESP_OK && bytes_read > 0) {
        tud_audio_write(itf, i2s_rx_buffer, bytes_read);
    }
}

// TX post-load callback
void tud_audio_tx_done_post_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep,
                                   uint8_t cur_alt_setting, uint16_t n_bytes_copied) {
    (void)rhport;
    (void)itf;
    (void)ep;
    (void)cur_alt_setting;
    (void)n_bytes_copied;
}

// RX callback (receive data from host)
void tud_audio_rx_done_read_cb(uint8_t rhport, uint8_t itf, uint8_t ep,
                              uint8_t cur_alt_setting, uint8_t *buffer, uint16_t bufsize) {
    (void)rhport;
    (void)itf;
    (void)ep;
    (void)cur_alt_setting;

    // Write to I2S to play audio
    size_t bytes_written = 0;
    i2s_write(I2S_NUM_0, buffer, bufsize, &bytes_written, 10 / portTICK_PERIOD_MS);
}

// Sample rate set callback
bool tud_audio_set_sample_rate_cb(uint8_t rhport, uint8_t itf, uint8_t alt_setting, uint32_t current_rate) {
    ESP_LOGI(TAG, "Sample rate set to %lu Hz (itf=%d, alt=%d)", current_rate, itf, alt_setting);
    return true;
}

// Mute control
bool tud_audio_get_mute_cb(uint8_t rhport, uint8_t itf, uint8_t cs, uint8_t channel) {
    (void)rhport;
    (void)itf;
    (void)cs;
    (void)channel;
    return false;
}

// Volume control
uint16_t tud_audio_get_volume_cb(uint8_t rhport, uint8_t itf, uint8_t cs, uint8_t channel) {
    (void)rhport;
    (void)itf;
    (void)cs;
    (void)channel;
    return 0x2000;  // 100% volume (0dB)
}

// Feature unit control
bool tud_audio_set_interface_cb(uint8_t rhport, uint8_t itf, uint8_t alt_itf) {
    ESP_LOGI(TAG, "Set interface %d to alt %d", itf, alt_itf);
    return true;
}

// Get descriptor (required by TinyUSB)
uint16_t tud_audio_n_get_len(uint8_t rhport, uint8_t func_id, uint8_t alt_itf, uint8_t itf, uint8_t ep) {
    (void)rhport;
    (void)func_id;

    if (itf == ITF_NUM_AUDIO_CONTROL && ep == 0) {
        return TUD_AUDIO_DESC_LEN;
    }
    if (itf == ITF_NUM_AUDIO_STREAMING_IN && ep == 0x81) {
        return EP_IN_SIZE;
    }
    if (itf == ITF_NUM_AUDIO_STREAMING_OUT && ep == 0x01) {
        return EP_OUT_SIZE;
    }
    return 0;
}

#endif // CONFIG_TINYUSB_ENABLED

//====================================================================
// I2S Configuration
//====================================================================

static void i2s_init(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_PIN,
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));

    ESP_LOGI(TAG, "I2S initialized: BCK=GPIO%d, WS=GPIO%d, DATA=GPIO%d",
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_IN_PIN);
    ESP_LOGI(TAG, "Audio: %dHz, %dch, %d-bit", AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BIT_DEPTH);
}

//====================================================================
// Main
//====================================================================

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 USB Audio Device");
    ESP_LOGI(TAG, "========================================");

#ifdef CONFIG_TINYUSB_ENABLED
    ESP_LOGI(TAG, "Installing TinyUSB with Audio...");

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = string_desc_arr,
        .external_phy = false,
        .configuration_descriptor = desc_configuration,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB Audio driver installed");

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Check Windows Device Manager:");
    ESP_LOGI(TAG, "  - Should show 'ESP32-S3 USB Audio' under Sound");
    ESP_LOGI(TAG, "  - Or under 'Universal Serial Bus devices'");
    ESP_LOGI(TAG, "");
#else
    ESP_LOGE(TAG, "TinyUSB not enabled in sdkconfig!");
#endif

    // Initialize I2S
    i2s_init();
    ESP_LOGI(TAG, "I2S initialized - audio bridge ready");

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Setup complete! Connect USB and check device.");
    ESP_LOGI(TAG, "========================================");
}
