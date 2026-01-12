#pragma once
#include <cstdint>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

class Lcd2004LiquidCrystalI2c
{
public:
    Lcd2004LiquidCrystalI2c(uint8_t addr, int cols=20, int rows=4);
    ~Lcd2004LiquidCrystalI2c();

    // Creates its own master bus on I2C_NUM_0 (GPIOs provided here)
    esp_err_t init(gpio_num_t sda, gpio_num_t scl, uint32_t freq_hz);

    void backlight(bool on);

    esp_err_t clear();
    esp_err_t setCursor(int col, int row);
    esp_err_t print(const char* s);
    esp_err_t printLine(int row, const char* s);

    // LiquidCrystal-compatible extras (needed for heart icon)
    esp_err_t createChar(uint8_t location, const uint8_t charmap[8]);
    esp_err_t writeChar(uint8_t ch);

private:
    esp_err_t command(uint8_t value);
    esp_err_t write(uint8_t value);
    esp_err_t send(uint8_t value, uint8_t mode);
    esp_err_t write4bits(uint8_t value);
    esp_err_t pulseEnable(uint8_t value);
    esp_err_t expander_write(uint8_t data);

private:
    uint8_t addr_;
    int cols_;
    int rows_;

    bool backlight_ = true;

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;

    // Persistent TX byte (safe even if async ever happens)
    uint8_t tx_byte_ = 0;
};
