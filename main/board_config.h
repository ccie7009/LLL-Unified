#pragma once
#include "driver/spi_master.h"

// ============================================================
// TARGET: ESP32-S3-ETH (W5500 SPI Ethernet)
// ============================================================
#if defined(CONFIG_IDF_TARGET_ESP32S3)

    // Ethernet - W5500 via SPI2
    #define ETH_TYPE_W5500
    #define ETH_SPI_HOST        SPI2_HOST
    #define ETH_SCLK_GPIO       13
    #define ETH_MOSI_GPIO       11
    #define ETH_MISO_GPIO       12
    #define ETH_CS_GPIO         14
    #define ETH_INT_GPIO        10
    #define ETH_RST_GPIO        9
    #define ETH_SPI_CLOCK_MHZ   25

    // Display - ST7789P3 via SPI3
    #define LCD_SPI_HOST        SPI3_HOST
    #define PIN_NUM_SCLK        48
    #define PIN_NUM_MOSI        1
    #define PIN_NUM_LCD_DC      42
    #define PIN_NUM_LCD_RST     41
    #define PIN_NUM_LCD_CS      47
    #define PIN_NUM_LCD_BCKL    2

    // Button
    #define PIN_NUM_SCRLSPD     GPIO_NUM_0
// ============================================================
// TARGET: ESP32-P4-ETH (IP101GRI RMII Ethernet)
// ============================================================
#elif defined(CONFIG_IDF_TARGET_ESP32P4)

    // Ethernet - IP101GRI via internal EMAC + RMII
    #define ETH_TYPE_EMAC_RMII
    #define ETH_MDC_GPIO        31
    #define ETH_MDIO_GPIO       52
    #define ETH_PHY_RST_GPIO    51
    #define ETH_PHY_ADDR        1       // IP101GRI default PHY address
    #define ETH_RMII_CLK_GPIO   50      // REF_CLK output from PHY

    // Display - ST7789P3 via SPI (use available GPIOs)
    // These are free GPIOs on P4-ETH based on schematic
    #define LCD_SPI_HOST        SPI2_HOST
    #define PIN_NUM_SCLK        15
    #define PIN_NUM_MOSI        16
    #define PIN_NUM_LCD_DC      18
    #define PIN_NUM_LCD_RST     17
    #define PIN_NUM_LCD_CS      19
    #define PIN_NUM_LCD_BCKL    54

    // Button
    #define PIN_NUM_SCRLSPD     GPIO_NUM_25
// ============================================================
// TARGET: ESP32 T-Internet-POE (LAN8720 RMII Ethernet)
// ============================================================
#elif defined(CONFIG_IDF_TARGET_ESP32)

    // Ethernet - LAN8720 via internal EMAC + RMII
    #define ETH_TYPE_EMAC_RMII
    #define ETH_MDC_GPIO        23
    #define ETH_MDIO_GPIO       18
    #define ETH_PHY_RST_GPIO    5    // no reset pin on T-Internet-POE
    #define ETH_PHY_ADDR        0    // LAN8720 default on T-Internet-POE
    #define ETH_CLK_MODE        ETH_CLOCK_GPIO17_OUT  // clock out on GPIO17
    #define ETH_PHY_POWER       17   // PHY power control

    // Display - ST7789P3 via SPI2
    #define LCD_SPI_HOST        SPI2_HOST
    #define PIN_NUM_SCLK        2
    #define PIN_NUM_MOSI        14
    #define PIN_NUM_LCD_DC      15
    #define PIN_NUM_LCD_RST     4
    #define PIN_NUM_LCD_CS      33
    #define PIN_NUM_LCD_BCKL    16

    // Button - GPIO32 is input-only, perfect for button
    #define PIN_NUM_SCRLSPD     GPIO_NUM_32
#else
    #error "Unsupported target. Select ESP32, ESP32-S3 or ESP32-P4 in menuconfig."
#endif

// ============================================================
// DISPLAY SETTINGS (same for all boards)
// ============================================================
    #define LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000) // 80 MHz
    #define LCD_H_RES       284
    #define LCD_V_RES       76
    #define LCD_X_OFFSET    18
    #define LCD_Y_OFFSET    82
