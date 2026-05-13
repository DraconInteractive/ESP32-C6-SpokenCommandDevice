#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/spi_types.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "display_node";

#define LCD_HOST SPI2_HOST
#define I2C_PORT I2C_NUM_0

#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_PIN_BL GPIO_NUM_3
#define LCD_PIN_DC GPIO_NUM_2
#define LCD_PIN_CS GPIO_NUM_10
#define LCD_PIN_CLK GPIO_NUM_6
#define LCD_PIN_MOSI GPIO_NUM_7
#define LCD_PIN_MISO (-1)
#define LCD_PIN_RST (-1)
#define TOUCH_PIN_RST GPIO_NUM_1
#define TOUCH_PIN_INT GPIO_NUM_0
#define I2C_PIN_SDA GPIO_NUM_4
#define I2C_PIN_SCL GPIO_NUM_5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRIES 5
#define EVENT_POLL_INTERVAL_US (5LL * 1000LL * 1000LL)
#define ALERT_DISPLAY_TIME_US (5LL * 1000LL * 1000LL)
#define HTTP_RESPONSE_MAX 1024
#define DISPLAY_TEXT_MAX 160

typedef struct {
    char data[HTTP_RESPONSE_MAX];
    int length;
} http_response_t;

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;
static bool s_wifi_ready;
static char s_device_id[48] = "waveshare-c3-display-unknown";
static esp_lcd_panel_io_handle_t s_lcd_io;
static SemaphoreHandle_t s_color_done;
static uint16_t s_line_buffer[LCD_H_RES];
static char s_display_text[DISPLAY_TEXT_MAX] = "Display ready.";

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = (uint16_t)(((b & 0xf8) << 8) | ((g & 0xfc) << 3) | (r >> 3));
    return (uint16_t)((color << 8) | (color >> 8));
}

static uint64_t gpio_pin_mask(int pin)
{
    return pin >= 0 ? (1ULL << pin) : 0;
}

static bool color_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    xSemaphoreGiveFromISR(sem, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t lcd_cmd(uint8_t cmd, const void *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_lcd_io, cmd, data, len);
}

static esp_err_t lcd_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t col[] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)((x1 - 1) >> 8), (uint8_t)(x1 - 1),
    };
    uint8_t row[] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1),
    };
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2a, col, sizeof(col)), TAG, "set column");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2b, row, sizeof(row)), TAG, "set row");
    return ESP_OK;
}

static esp_err_t lcd_write_color(int x0, int y0, int x1, int y1, const uint16_t *color, size_t bytes)
{
    ESP_RETURN_ON_ERROR(lcd_set_window(x0, y0, x1, y1), TAG, "set draw window");
    xSemaphoreTake(s_color_done, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_lcd_io, 0x2c, color, bytes), TAG, "write color");
    xSemaphoreTake(s_color_done, pdMS_TO_TICKS(1000));
    return ESP_OK;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > LCD_H_RES ? LCD_H_RES : x1;
    y1 = y1 > LCD_V_RES ? LCD_V_RES : y1;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (int x = 0; x < x1 - x0; ++x) {
        s_line_buffer[x] = color;
    }
    for (int y = y0; y < y1; ++y) {
        ESP_ERROR_CHECK(lcd_write_color(x0, y, x1, y + 1, s_line_buffer, (x1 - x0) * sizeof(uint16_t)));
    }
}

static void lcd_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_CLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = color_done_cb,
        .user_ctx = NULL,
    };
    uint64_t output_pins = gpio_pin_mask(TOUCH_PIN_RST) |
                           gpio_pin_mask(LCD_PIN_RST) |
                           gpio_pin_mask(LCD_PIN_BL);

    s_color_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_color_done ? ESP_OK : ESP_ERR_NO_MEM);

    gpio_config_t gpio_out = {
        .pin_bit_mask = output_pins,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_out));
    if (LCD_PIN_BL >= 0) {
        gpio_set_level(LCD_PIN_BL, 0);
    }
    if (LCD_PIN_RST >= 0) {
        gpio_set_level(LCD_PIN_RST, 0);
    }
    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (LCD_PIN_RST >= 0) {
        gpio_set_level(LCD_PIN_RST, 1);
    }
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t cfg = io_config;
    cfg.user_ctx = s_color_done;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &s_lcd_io));

    ESP_ERROR_CHECK(lcd_cmd(0x01, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(lcd_cmd(0x11, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t madctl = 0x00;
    uint8_t colmod = 0x55;
    ESP_ERROR_CHECK(lcd_cmd(0x36, &madctl, 1));
    ESP_ERROR_CHECK(lcd_cmd(0x3a, &colmod, 1));
    ESP_ERROR_CHECK(lcd_cmd(0x21, NULL, 0));
    ESP_ERROR_CHECK(lcd_cmd(0x29, NULL, 0));
    if (LCD_PIN_BL >= 0) {
        gpio_set_level(LCD_PIN_BL, 1);
    }
}

static void i2c_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    gpio_config_t touch_int = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &config));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0));
    ESP_ERROR_CHECK(gpio_config(&touch_int));
}

static bool touch_pressed(void)
{
    return gpio_get_level(TOUCH_PIN_INT) == 0;
}

static const uint8_t *glyph(char c)
{
    static const uint8_t blank[5] = {0};
    static const uint8_t bang[5] = {0x00, 0x00, 0x5f, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t qmark[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t nums[][5] = {
        {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
        {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
        {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
    };
    static const uint8_t letters[][5] = {
        {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
        {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
        {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
        {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
        {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
        {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
        {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
        {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
        {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
        {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    };
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }
    if (c >= '0' && c <= '9') {
        return nums[c - '0'];
    }
    if (c == '!') return bang;
    if (c == '-') return dash;
    if (c == '.') return dot;
    if (c == ':') return colon;
    if (c == '?') return qmark;
    return blank;
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    const uint8_t *g = glyph(c);
    for (int py = 0; py < 7 * scale; ++py) {
        int gy = py / scale;
        for (int px = 0; px < 5 * scale; ++px) {
            int gx = px / scale;
            s_line_buffer[px] = (g[gx] & (1U << gy)) ? fg : bg;
        }
        ESP_ERROR_CHECK(lcd_write_color(x, y + py, x + 5 * scale, y + py + 1, s_line_buffer, 5 * scale * sizeof(uint16_t)));
    }
}

static int text_line_width(const char *text, int len, int scale)
{
    if (len <= 0) {
        return 0;
    }
    return ((len - 1) * 6 + 5) * scale;
}

static void draw_text_centered(int y, int max_width, int max_lines, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    const int char_w = 6 * scale;
    const int max_chars = max_width / char_w;
    const char *cursor = text;

    for (int line = 0; line < max_lines && *cursor != '\0'; ++line) {
        while (*cursor == ' ') {
            cursor++;
        }

        int len = 0;
        int last_space = -1;
        while (cursor[len] != '\0' && cursor[len] != '\n' && len < max_chars) {
            if (cursor[len] == ' ') {
                last_space = len;
            }
            len++;
        }
        if (cursor[len] != '\0' && cursor[len] != '\n' && last_space > 0) {
            len = last_space;
        }

        int x = (LCD_H_RES - text_line_width(cursor, len, scale)) / 2;
        for (int i = 0; i < len; ++i) {
            draw_char(x + i * char_w, y, cursor[i], fg, bg, scale);
        }

        cursor += len;
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\n') {
            cursor++;
        }
        y += 9 * scale;
    }
}

static void draw_alert_icon(void)
{
    const int cx = LCD_H_RES / 2;
    const int cy = 82;
    const int radius = 36;
    const uint16_t red = rgb565(225, 52, 60);
    const uint16_t white = rgb565(255, 246, 232);
    for (int y = -radius; y < radius; ++y) {
        int half = 0;
        while ((half + 1) * (half + 1) + y * y <= radius * radius) {
            half++;
        }
        fill_rect(cx - half, cy + y, cx + half, cy + y + 1, red);
    }
    fill_rect(cx - 4, cy - 21, cx + 4, cy + 10, white);
    fill_rect(cx - 4, cy + 20, cx + 4, cy + 28, white);
}

static void render_home(void)
{
    uint16_t bg = rgb565(6, 10, 14);
    uint16_t fg = rgb565(218, 232, 226);
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, bg);
    draw_text_centered(96, 156, 3, s_display_text, fg, bg, 1);
}

static void render_alert(const char *text)
{
    int64_t until = esp_timer_get_time() + ALERT_DISPLAY_TIME_US;
    uint16_t bg = rgb565(6, 10, 14);
    uint16_t fg = rgb565(255, 246, 232);
    strlcpy(s_display_text, text && text[0] ? text : "Alert", sizeof(s_display_text));
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, bg);
    draw_alert_icon();
    draw_text_centered(136, 172, 5, s_display_text, fg, bg, 1);
    while (esp_timer_get_time() < until) {
        if (touch_pressed()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    render_home();
}

static void init_device_id(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_device_id, sizeof(s_device_id), "waveshare-c3-display-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ready = false;
        if (s_wifi_retry_count < WIFI_MAX_RETRIES) {
            s_wifi_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        s_wifi_ready = true;
        ESP_LOGI(TAG, "Wi-Fi connected ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    if (strlen(CONFIG_DISPLAY_NODE_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "Wi-Fi disabled: DISPLAY_NODE_WIFI_SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create Wi-Fi event group");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_DISPLAY_NODE_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_DISPLAY_NODE_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(CONFIG_DISPLAY_NODE_WIFI_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    return s_wifi_ready ? ESP_OK : ESP_FAIL;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && response != NULL && evt->data_len > 0) {
        int copy = evt->data_len;
        if (copy > HTTP_RESPONSE_MAX - response->length - 1) {
            copy = HTTP_RESPONSE_MAX - response->length - 1;
        }
        if (copy > 0) {
            memcpy(response->data + response->length, evt->data, copy);
            response->length += copy;
            response->data[response->length] = '\0';
        }
    }
    return ESP_OK;
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[40] = {0};
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos = strchr(pos + strlen(pattern), ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return false;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i + 1 < out_size) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i > 0;
}

static void poll_events(void)
{
    if (!s_wifi_ready) {
        return;
    }
    char url[192] = {0};
    http_response_t response = {0};
    snprintf(url, sizeof(url), "%s/devices/%s/events", CONFIG_DISPLAY_NODE_SERVER_URL, s_device_id);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 2500,
        .event_handler = http_event_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return;
    }
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK || status < 200 || status >= 300 || strstr(response.data, "\"type\"") == NULL) {
        return;
    }

    char type[24] = {0};
    char display_text[DISPLAY_TEXT_MAX] = {0};
    extract_json_string(response.data, "type", type, sizeof(type));
    extract_json_string(response.data, "display_text", display_text, sizeof(display_text));
    ESP_LOGI(TAG, "Event type=%s display=%s", type, display_text);
    if (strcmp(type, "alert") == 0) {
        render_alert(display_text);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    init_device_id();
    i2c_init();
    lcd_init();
    render_home();
    if (wifi_init_sta() != ESP_OK) {
        strlcpy(s_display_text, "Wi-Fi not connected.", sizeof(s_display_text));
        render_home();
    }

    int64_t next_poll = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now >= next_poll) {
            next_poll = now + EVENT_POLL_INTERVAL_US;
            poll_events();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
