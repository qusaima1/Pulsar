#include "lcd2004_liquidcrystal_i2c.h"

#include <cstring>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static inline void delay_us(uint32_t us) { esp_rom_delay_us(us); }

// LiquidCrystal_I2C mapping (PCF8574):
// RS=P0, RW=P1, EN=P2, BL=P3, D4..D7=P4..P7
static constexpr uint8_t MASK_RS = 0x01;
static constexpr uint8_t MASK_RW = 0x02;
static constexpr uint8_t MASK_EN = 0x04;
static constexpr uint8_t MASK_BL = 0x08;

// Commands
static constexpr uint8_t LCD_CLEARDISPLAY   = 0x01;
static constexpr uint8_t LCD_RETURNHOME     = 0x02;
static constexpr uint8_t LCD_ENTRYMODESET   = 0x04;
static constexpr uint8_t LCD_DISPLAYCONTROL = 0x08;
static constexpr uint8_t LCD_FUNCTIONSET    = 0x20;
static constexpr uint8_t LCD_SETCGRAMADDR   = 0x40;
static constexpr uint8_t LCD_SETDDRAMADDR   = 0x80;

// Flags
static constexpr uint8_t LCD_ENTRYLEFT           = 0x02;
static constexpr uint8_t LCD_ENTRYSHIFTDECREMENT = 0x00;

static constexpr uint8_t LCD_DISPLAYON  = 0x04;
static constexpr uint8_t LCD_CURSOROFF  = 0x00;
static constexpr uint8_t LCD_BLINKOFF   = 0x00;

static constexpr uint8_t LCD_4BITMODE = 0x00;
static constexpr uint8_t LCD_2LINE    = 0x08;
static constexpr uint8_t LCD_5x8DOTS  = 0x00;

Lcd2004LiquidCrystalI2c::Lcd2004LiquidCrystalI2c(uint8_t addr, int cols, int rows)
: addr_(addr), cols_(cols), rows_(rows) {}

Lcd2004LiquidCrystalI2c::~Lcd2004LiquidCrystalI2c()
{
    if (bus_) {
        if (dev_) {
            i2c_master_bus_rm_device(dev_);
            dev_ = nullptr;
        }
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }
}

esp_err_t Lcd2004LiquidCrystalI2c::init(gpio_num_t sda, gpio_num_t scl, uint32_t freq_hz)
{
    // Recreate cleanly (avoids "driver install" state issues)
    if (bus_) {
        if (dev_) {
            i2c_master_bus_rm_device(dev_);
            dev_ = nullptr;
        }
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = sda;
    bus_cfg.scl_io_num = scl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority = 0;

    // Critical: keep synchronous mode for LCD (prevents queue overflow + async lifetime bugs)
    bus_cfg.trans_queue_depth = 0;

    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr_;
    dev_cfg.scl_speed_hz = freq_hz;

    err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) return err;

    // Power-up settle
    backlight_ = true;
    tx_byte_ = 0;
    err = expander_write(0x00);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(80));

    // 4-bit init sequence (LiquidCrystal_I2C style)
    err = write4bits(0x03 << 4); if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = write4bits(0x03 << 4); if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = write4bits(0x03 << 4); if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));
    err = write4bits(0x02 << 4); if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t function = LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS;
    err = command(function); if (err != ESP_OK) return err;

    uint8_t display = LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
    err = command(display); if (err != ESP_OK) return err;

    err = clear(); if (err != ESP_OK) return err;

    uint8_t entry = LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
    err = command(entry); if (err != ESP_OK) return err;

    err = command(LCD_RETURNHOME); if (err != ESP_OK) return err;
    delay_us(2000);

    return ESP_OK;
}

void Lcd2004LiquidCrystalI2c::backlight(bool on)
{
    backlight_ = on;
    (void)expander_write(0x00);
}

esp_err_t Lcd2004LiquidCrystalI2c::clear()
{
    esp_err_t err = command(LCD_CLEARDISPLAY);
    delay_us(2000);
    return err;
}

esp_err_t Lcd2004LiquidCrystalI2c::setCursor(int col, int row)
{
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    row = std::clamp(row, 0, rows_ - 1);
    col = std::clamp(col, 0, cols_ - 1);
    return command((uint8_t)(LCD_SETDDRAMADDR | (row_offsets[row] + col)));
}

esp_err_t Lcd2004LiquidCrystalI2c::print(const char* s)
{
    if (!s) return ESP_OK;
    while (*s) {
        esp_err_t err = write((uint8_t)*s++);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t Lcd2004LiquidCrystalI2c::printLine(int row, const char* s)
{
    char buf[21];
    std::memset(buf, ' ', 20);
    buf[20] = '\0';

    if (s) {
        size_t n = std::min((size_t)20, std::strlen(s));
        std::memcpy(buf, s, n);
    }

    esp_err_t err = setCursor(0, row);
    if (err != ESP_OK) return err;
    return print(buf);
}

esp_err_t Lcd2004LiquidCrystalI2c::createChar(uint8_t location, const uint8_t charmap[8])
{
    location &= 0x7; // 0..7
    esp_err_t err = command((uint8_t)(LCD_SETCGRAMADDR | (location << 3)));
    if (err != ESP_OK) return err;

    for (int i = 0; i < 8; ++i) {
        err = write(charmap[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t Lcd2004LiquidCrystalI2c::writeChar(uint8_t ch)
{
    return write(ch);
}

esp_err_t Lcd2004LiquidCrystalI2c::command(uint8_t value) { return send(value, 0); }
esp_err_t Lcd2004LiquidCrystalI2c::write(uint8_t value)   { return send(value, MASK_RS); }

esp_err_t Lcd2004LiquidCrystalI2c::send(uint8_t value, uint8_t mode)
{
    uint8_t high = (uint8_t)(value & 0xF0);
    uint8_t low  = (uint8_t)((value << 4) & 0xF0);

    esp_err_t err = write4bits((uint8_t)(high | mode));
    if (err != ESP_OK) return err;
    return write4bits((uint8_t)(low | mode));
}

esp_err_t Lcd2004LiquidCrystalI2c::write4bits(uint8_t value)
{
    esp_err_t err = expander_write(value);
    if (err != ESP_OK) return err;
    return pulseEnable(value);
}

esp_err_t Lcd2004LiquidCrystalI2c::pulseEnable(uint8_t value)
{
    esp_err_t err = expander_write((uint8_t)(value | MASK_EN));
    if (err != ESP_OK) return err;
    delay_us(1);

    err = expander_write((uint8_t)(value & (uint8_t)~MASK_EN));
    if (err != ESP_OK) return err;

    delay_us(80);
    return ESP_OK;
}

esp_err_t Lcd2004LiquidCrystalI2c::expander_write(uint8_t data)
{
    data &= (uint8_t)~MASK_RW; // RW always low
    if (backlight_) data |= MASK_BL;
    else            data &= (uint8_t)~MASK_BL;

    tx_byte_ = data; // persistent buffer
    return i2c_master_transmit(dev_, &tx_byte_, 1, 200 /*ms*/);
}
