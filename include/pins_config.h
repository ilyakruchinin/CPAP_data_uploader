#ifndef CONFIG_H
#define CONFIG_H

// Pin definitions for ESP32 PICO D4 (SD WIFI PRO)
#define LED_BUILTIN 2

// SD WIFI PRO Pin Mapping
// SDIO Pins (Built-in 8GB Flash)
#define SDIO_D3_CS    // Pin 1 - SPI CS
#define SDIO_CMD_MOSI // Pin 2 - SPI MOSI
#define SDIO_CLK      // Pin 5 - SPI CLK
#define SDIO_D0_MISO  // Pin 7 - SPI MISO

// GPIO Pins Available
#define GPIO_32  32  // Pin 10 - ADC1_CH4, TOUCH9, RTC_GPIO9
#define GPIO_26  26  // Pin 11 - DAC_2, ADC2_CH9, RTC_GPIO7
#define GPIO_2   2   // Pin 12 - Must be LOW for flashing
#define GPIO_0   0   // Pin 13 - Must be LOW for flashing, outputs PWM at boot
#define GPIO_3   3   // Pin 15 - RX0, HIGH at boot
#define GPIO_1   1   // Pin 16 - TX0, debug output at boot
#define GPIO_22  22  // Pin 17 - I2C_SCL
#define GPIO_21  21  // Pin 18 - I2C_SDA
#define GPIO_19  19  // Pin 19

// Serial pins
#define RX0 GPIO_3
#define TX0 GPIO_1

// I2C pins
#define I2C_SCL GPIO_22
#define I2C_SDA GPIO_21

#endif
