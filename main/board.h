#pragma once

// ===== ESP-VoCat v1.2 (EchoEar) Pin Definitions =====

// I2C (pour ES8311 / ES7210)
#define BOARD_I2C_SDA       GPIO_NUM_2
#define BOARD_I2C_SCL       GPIO_NUM_1

// I2S Audio
#define BOARD_I2S_MCLK      GPIO_NUM_42
#define BOARD_I2S_BCLK      GPIO_NUM_40
#define BOARD_I2S_WS        GPIO_NUM_39
#define BOARD_I2S_DOUT      GPIO_NUM_41
#define BOARD_I2S_DIN       GPIO_NUM_3

// Amplificateur & Codec Power
#define BOARD_PA_CTRL       GPIO_NUM_15
#define BOARD_CODEC_PWR     GPIO_NUM_48

// LED verte (MicBoard)
#define BOARD_LED_GPIO      GPIO_NUM_43

// LCD (ST77916 QSPI) - pins from official ESP-VoCat BSP
#define BOARD_LCD_CLK       GPIO_NUM_18
#define BOARD_LCD_D0        GPIO_NUM_46
#define BOARD_LCD_D1        GPIO_NUM_13
#define BOARD_LCD_D2        GPIO_NUM_11
#define BOARD_LCD_D3        GPIO_NUM_12
#define BOARD_LCD_DC        GPIO_NUM_45
#define BOARD_LCD_CS        GPIO_NUM_14
#define BOARD_LCD_RST       GPIO_NUM_47
#define BOARD_LCD_BL        GPIO_NUM_44
#define BOARD_LCD_EN        GPIO_NUM_9   // Active LOW: enable LCD power

// ES8311 I2C Address
#define ES8311_I2C_ADDR     0x18

// I2S Peripherals
#define BOARD_I2S_NUM       I2S_NUM_0   // ES8311 DAC (speaker) - STD mode TX
#define BOARD_I2S_NUM_MIC   I2S_NUM_1   // ES7210 ADC (micros)  - TDM mode RX
