#include "ano_seesaw.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ano";

#define ANO_ADDR 0x49
#define ANO_PRODUCT_ID 5740
#define I2C_TIMEOUT_MS 20
// The seesaw needs time to stage a register before it can be read back.
#define READ_DELAY_US 250

// Seesaw module bases and registers (Adafruit seesaw protocol)
#define SS_STATUS 0x00
#define SS_STATUS_VERSION 0x02
#define SS_STATUS_SWRST 0x7F
#define SS_GPIO 0x01
#define SS_GPIO_DIRCLR_BULK 0x03
#define SS_GPIO_BULK 0x04
#define SS_GPIO_BULK_SET 0x05
#define SS_GPIO_PULLENSET 0x0B
#define SS_ENCODER 0x11
#define SS_ENCODER_DELTA 0x40

#define BUTTON_MASK (ANO_BTN_SELECT | ANO_BTN_UP | ANO_BTN_LEFT | \
                     ANO_BTN_DOWN | ANO_BTN_RIGHT)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

// Undo a partial init so a later ano_seesaw_init() can retry cleanly.
static bool give_up(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return false;
}

static esp_err_t ss_write(uint8_t base, uint8_t reg, const uint8_t *data,
                          size_t len)
{
    uint8_t buf[6] = {base, reg};
    for (size_t i = 0; i < len; i++) {
        buf[2 + i] = data[i];
    }
    return i2c_master_transmit(s_dev, buf, 2 + len, I2C_TIMEOUT_MS);
}

static esp_err_t ss_read(uint8_t base, uint8_t reg, uint8_t *out, size_t len)
{
    esp_err_t err = ss_write(base, reg, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(READ_DELAY_US);
    return i2c_master_receive(s_dev, out, len, I2C_TIMEOUT_MS);
}

static esp_err_t ss_read_u32(uint8_t base, uint8_t reg, uint32_t *out)
{
    uint8_t b[4];
    esp_err_t err = ss_read(base, reg, b, sizeof(b));
    if (err == ESP_OK) {
        *out = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
               (uint32_t)b[2] << 8 | b[3];
    }
    return err;
}

static esp_err_t ss_write_u32(uint8_t base, uint8_t reg, uint32_t v)
{
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    return ss_write(base, reg, b, sizeof(b));
}

bool ano_seesaw_init(int sda_pin, int scl_pin)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The adapter has its own pull-ups; these keep an empty bus from
        // floating so the probe fails cleanly.
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_master_probe(s_bus, ANO_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        // NOT_FOUND = nothing acked; TIMEOUT = a line is stuck (shorted).
        ESP_LOGI(TAG, "no ANO encoder at 0x%02x (%s)", ANO_ADDR,
                 esp_err_to_name(err));
        return give_up();
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ANO_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return give_up();
    }

    uint8_t rst = 0xFF;
    ss_write(SS_STATUS, SS_STATUS_SWRST, &rst, 1);

    // The seesaw takes a variable time to come back from reset, so poll
    // for it rather than trusting a fixed delay.
    uint32_t version = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = ss_read_u32(SS_STATUS, SS_STATUS_VERSION, &version);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ANO encoder not responding after reset (%s)",
                 esp_err_to_name(err));
        return give_up();
    }
    // The upper half of the version register is the Adafruit product id.
    if ((version >> 16) != ANO_PRODUCT_ID) {
        ESP_LOGW(TAG, "seesaw at 0x%02x is product %lu, not the ANO (%d)",
                 ANO_ADDR, (unsigned long)(version >> 16), ANO_PRODUCT_ID);
        return give_up();
    }

    // Buttons are inputs with pull-ups (pressed = low).
    if (ss_write_u32(SS_GPIO, SS_GPIO_DIRCLR_BULK, BUTTON_MASK) != ESP_OK ||
        ss_write_u32(SS_GPIO, SS_GPIO_PULLENSET, BUTTON_MASK) != ESP_OK ||
        ss_write_u32(SS_GPIO, SS_GPIO_BULK_SET, BUTTON_MASK) != ESP_OK) {
        ESP_LOGW(TAG, "ANO button setup failed");
        return give_up();
    }

    // Discard any rotation accumulated before we started listening.
    int32_t discard;
    ano_seesaw_read_delta(&discard);

    ESP_LOGI(TAG, "ANO encoder ready");
    return true;
}

esp_err_t ano_seesaw_read_buttons(uint32_t *pressed)
{
    uint32_t levels;
    esp_err_t err = ss_read_u32(SS_GPIO, SS_GPIO_BULK, &levels);
    if (err == ESP_OK) {
        *pressed = ~levels & BUTTON_MASK;
    }
    return err;
}

esp_err_t ano_seesaw_read_delta(int32_t *delta)
{
    uint32_t raw;
    esp_err_t err = ss_read_u32(SS_ENCODER, SS_ENCODER_DELTA, &raw);
    if (err == ESP_OK) {
        // The seesaw counts counter-clockwise as positive.
        *delta = -(int32_t)raw;
    }
    return err;
}
