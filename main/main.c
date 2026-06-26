#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/semphr.h"

#include "lldp_cdp_parser.h"
#include "board_config.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_eth_phy_802_3.h"
#include "esp_eth_mac_esp.h"
#endif

#define SCROLL_SLOW         50
#define SCROLL_FAST         200
#define DEBOUNCE_TIME_MS    200

// Multicast MACs
static const uint8_t lldp_mac[] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};
static const uint8_t cdp_mac[]  = {0x01, 0x00, 0x0C, 0xCC, 0xCC, 0xCC};
static const uint8_t edp_mac[]  = {0x00, 0xE0, 0x2B, 0x00, 0x00, 0x00};
static const uint8_t fdp_mac[]  = {0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC};
static const uint8_t ndp_mac[]  = {0x01, 0x00, 0x81, 0x00, 0x01, 0x00};
// MNDP uses broadcast FF:FF:FF:FF:FF:FF detected via UDP port 5678

#define ETHERTYPE_LLDP 0x88CC
#define ETHERTYPE_CDP 0x2000

#define USE_CIRCLE_INDICATOR    // Comment out to use bar indicator instead of circle
    
typedef struct {
    proto_type_t proto;
    neighbor_info_t info;
} parsed_packet_t;

static esp_lcd_panel_handle_t panel_handle = NULL;
static lv_disp_drv_t disp_drv;
static QueueSetHandle_t gpio_evt_queue = NULL;
lv_obj_t *ProtocolLabel;
lv_obj_t *HostLabel;
lv_obj_t *InterfaceLabel;
lv_obj_t *VlanLabel;
lv_obj_t *PlatformLabel;
SemaphoreHandle_t lvgl_mutex;
static QueueHandle_t packet_queue = NULL;

lv_obj_t *SpeedIndicator;
static uint32_t scroll_speed = 200; // Initial scroll speed in milliseconds

static void IRAM_ATTR gpio_isr_handler(void *arg) {
	uint32_t gpio_num = (uint32_t)arg;
	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_scroll_speed_task(void *arg) {
	uint32_t gpio_num;
	int64_t last_time = 0;

    //ESP_LOGI("TEST", "GPIO scroll speed task started");

	for (;;) {
		if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY) == pdTRUE)
		{
            //ESP_LOGI("TEST", "GPIO[%ld] intr received", gpio_num);
			int64_t now = esp_timer_get_time() / 1000; // Convert to milliseconds
            //ESP_LOGI("TEST", "Current time: %lld ms, Last time: %lld ms difference: %lld ms", now, last_time, now - last_time);
			if (now - last_time > DEBOUNCE_TIME_MS) {
				last_time = now;
				//ESP_LOGI("TEST", "Current scroll speed: %ld ms", scroll_speed);
				uint32_t new_speed = scroll_speed * 2; // SLOW, MEDIUM, FAST cycle
				if (new_speed > SCROLL_FAST)
					new_speed = SCROLL_SLOW;
				scroll_speed = new_speed;
                uint8_t circle_size;
                if (new_speed == SCROLL_SLOW) {
                    circle_size = 4; // Small circle for slow
                } else if (new_speed == SCROLL_FAST) {
                    circle_size = 12; // Large circle for fast
                } else {
                    circle_size = 8; // Medium circle for medium
                }
				//ESP_LOGI("TEST", "GPIO[%ld] intr, set scroll speed to %ld ms", gpio_num, new_speed);
                if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
				    lv_obj_set_style_anim_speed(HostLabel, new_speed, LV_PART_MAIN);
				    lv_obj_set_style_anim_speed(InterfaceLabel, new_speed, LV_PART_MAIN);
#if defined(USE_CIRCLE_INDICATOR)
                    lv_obj_set_size(SpeedIndicator, circle_size, circle_size);
                    lv_obj_set_style_bg_color(SpeedIndicator, new_speed == SCROLL_FAST ? lv_color_hex(0xFF0000) : new_speed == SCROLL_SLOW ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFF00), LV_PART_MAIN);
#else
                    lv_bar_set_value(SpeedIndicator, new_speed, LV_ANIM_ON);
                    lv_obj_set_style_bg_color(SpeedIndicator, new_speed == SCROLL_FAST ? lv_color_hex(0xFF0000) : new_speed == SCROLL_SLOW ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
#endif
			    	lv_label_set_long_mode(HostLabel, LV_LABEL_LONG_CLIP); // Set to clip to stop scrolling when speed changes
		    		lv_label_set_long_mode(InterfaceLabel, LV_LABEL_LONG_CLIP); // Set to clip to stop scrolling when speed changes
	    			lv_label_set_long_mode(HostLabel, LV_LABEL_LONG_SCROLL_CIRCULAR); // Set back to circular scroll
    				lv_label_set_long_mode(InterfaceLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
                    xSemaphoreGive(lvgl_mutex);
                    //ESP_LOGI("TEST", "Mutex released, speed set to %lu", new_speed);
                //} else {
                    //ESP_LOGW("TEST", "Failed to take mutex within 50ms");
                }
			}
            //ESP_LOGI("TEST", "GPIO[%lu] scroll speed set", gpio_num);
		//} else {
            //ESP_LOGE("TEST", "Debounce filtered - diff too small");
        }   
	}
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    //lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(&disp_drv);
    return false;
}

void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

// 1. Setup a periodic timer for the Tick
static void lv_tick_task(void *arg) {
    lv_tick_inc(10); // Tell LVGL 10ms have passed
}

static esp_err_t eth_recv_callback(esp_eth_handle_t hdl,
                                    uint8_t *buffer,
                                    uint32_t length,
                                    void *priv) {
    if (length < 14) {
        free(buffer);
        return ESP_OK;
    }

    uint8_t *dst_mac   = buffer;
    uint16_t protocol = (buffer[20] << 8) | buffer[21];
    uint16_t ethertype = (buffer[12] << 8) | buffer[13];

    parsed_packet_t pkt;
    bool parsed = false;

    if (memcmp(dst_mac, lldp_mac, 6) == 0 && ethertype == ETHERTYPE_LLDP) {
        pkt.proto = PROTO_LLDP;
        ESP_LOGI("ETH", "Received LLDP packet, length: %ld", length);
        parsed = parse_lldp(buffer, length, &pkt.info);
    } else if (memcmp(dst_mac, cdp_mac, 6) == 0 && protocol == ETHERTYPE_CDP) {
        pkt.proto = PROTO_CDP;
        ESP_LOGI("ETH", "Received CDP packet, length: %ld", length);
        parsed = parse_cdp(buffer, length, &pkt.info);
    } else if (memcmp(dst_mac, edp_mac, 6) == 0) {
        pkt.proto = PROTO_EDP;
        ESP_LOGI("ETH", "Received EDP packet, length: %ld", length);
        parsed = parse_edp(buffer, length, &pkt.info);

    } else if (memcmp(dst_mac, fdp_mac, 6) == 0) {
        pkt.proto = PROTO_FDP;
        ESP_LOGI("ETH", "Received FDP packet, length: %ld", length);
        parsed = parse_fdp(buffer, length, &pkt.info);

    } else if (memcmp(dst_mac, ndp_mac, 6) == 0) {
        pkt.proto = PROTO_NDP;
        parsed = parse_ndp(buffer, length, &pkt.info);

    } else if (ethertype == 0x0800) {
        // Check for MNDP - IPv4 UDP broadcast to port 5678
        if (length > 42) {
            const uint8_t *ip = buffer + 14;
            uint8_t ihl = (ip[0] & 0x0F) * 4;
            const uint8_t *udp = ip + ihl;
            if (ip[9] == 0x11) {  // UDP
                uint16_t dst_port = (udp[2] << 8) | udp[3];
                if (dst_port == 5678) {
                    pkt.proto = PROTO_MNDP;
                    ESP_LOGI("ETH", "Received MNDP packet, length: %ld", length);
                    parsed = parse_mndp(buffer, length, &pkt.info);
                }
            }
        }
    }

    if (parsed && pkt.info.valid) {
        // xQueueOverwrite so we always keep the latest per protocol
        // Use two separate queues — one per protocol
        ESP_LOGI("ETH", "[%s] host=%s port=%s vlan=%s platform=%s",
                 proto_name(pkt.proto),
                 pkt.info.hostname, pkt.info.port,
                 pkt.info.vlan, pkt.info.platform);
        xQueueOverwrite(packet_queue, &pkt);
    }

    free(buffer);
    return ESP_OK;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI("ETH", "Ethernet Link Up");
            lv_label_set_text(ProtocolLabel, "Ethernet Link Up");
            lv_obj_set_style_text_color(ProtocolLabel, lv_color_hex(0x00FF00), LV_PART_MAIN); // green
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI("ETH", "Ethernet Link Down");
            lv_label_set_text(ProtocolLabel, "Ethernet Link Down");
            lv_obj_set_style_text_color(ProtocolLabel, lv_color_hex(0xFF0000), LV_PART_MAIN); // red
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI("ETH", "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI("ETH", "Ethernet Stopped");
            break;
    }
}

static void eth_capture_init(void) {
    esp_eth_handle_t eth_handle = NULL;

#if defined(ETH_TYPE_W5500)
    // ── S3: W5500 via SPI ────────────────────────────────────
    spi_bus_config_t buscfg = {
        .miso_io_num   = ETH_MISO_GPIO,
        .mosi_io_num   = ETH_MOSI_GPIO,
        .sclk_io_num   = ETH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .mode           = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num   = ETH_CS_GPIO,
        .queue_size     = 20,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = ETH_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

#elif defined(ETH_TYPE_EMAC_RMII)
    // ── P4 + ESP32: Internal EMAC + RMII ─────────────────────

    #if defined(CONFIG_IDF_TARGET_ESP32)
    // T-Internet-POE: power cycle LAN8720 via GPIO17 before init
    esp_rom_gpio_pad_select_gpio(ETH_PHY_POWER);
    gpio_set_direction(ETH_PHY_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_PHY_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(ETH_PHY_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    #endif

    // EMAC config - clock mode comes from sdkconfig for ESP32,
    // set explicitly for P4
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    #if defined(CONFIG_IDF_TARGET_ESP32P4)
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;
    #endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    // PHY config - chip differs between P4 (IP101) and ESP32 (LAN8720)
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    #if defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    #elif defined(CONFIG_IDF_TARGET_ESP32)
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    #endif

#endif  // ETH_TYPE

    // ── Common init for all targets ───────────────────────────
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_update_input_path(eth_handle,
                                               eth_recv_callback, NULL));

    uint8_t mac_addr[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    bool promiscuous = true;
    esp_eth_ioctl(eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous);

    ESP_LOGI("ETH", "Ethernet capture started");
}

static void display_update_task(void *arg) {
    parsed_packet_t pkt;
    char vlan_str[40];

    while (1) {
        if (xQueueReceive(packet_queue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                lv_color_t color;
                switch (pkt.proto) {
                    case PROTO_LLDP: color = lv_color_hex(0xFF00FF); break; // magenta
                    case PROTO_CDP:  color = lv_color_hex(0x00BFFF); break; // blue
                    case PROTO_EDP:  color = lv_color_hex(0xFF8000); break; // orange
                    case PROTO_FDP:  color = lv_color_hex(0x8000FF); break; // purple
                    case PROTO_NDP:  color = lv_color_hex(0x00FFFF); break; // cyan
                    case PROTO_MNDP: color = lv_color_hex(0xFFFF00); break; // yellow
                    default:         color = lv_color_hex(0xFFFFFF); break; // white
                };
                lv_label_set_text(ProtocolLabel, proto_name(pkt.proto));
                lv_obj_set_style_text_color(ProtocolLabel, color, LV_PART_MAIN);

                // Update hostname
                lv_label_set_text(HostLabel,
                    pkt.info.hostname[0] ? pkt.info.hostname : "Unknown");
                
                // Update interface/port
                lv_label_set_text(InterfaceLabel,
                    pkt.info.port[0] ? pkt.info.port : "Unknown");

                // Update VLAN
                if (pkt.info.vlan[0]) {
                    snprintf(vlan_str, sizeof(vlan_str), 
                             "%s", pkt.info.vlan);
                } else {
                    snprintf(vlan_str, sizeof(vlan_str), "Unkn");
                }
                lv_label_set_text(VlanLabel, vlan_str);
                lv_obj_set_style_text_color(VlanLabel, color, LV_PART_MAIN);

                // Update platform/Model
                lv_label_set_text(PlatformLabel, pkt.info.platform[0] ? pkt.info.platform : "Unknown Model");
                lv_obj_set_style_text_color(PlatformLabel, color, LV_PART_MAIN);

                xSemaphoreGive(lvgl_mutex);
            }
        }
    }
}

void app_main(void)
{
    // *** SPI BUS ***
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
    };
    ESP_LOGI("INIT", "Initializing SPI bus...");
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // *** PANEL IO ***
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .spi_mode = 3,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv, // will be set to disp_driver later
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    ESP_LOGI("INIT", "Initializing PANEL IO...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    // *** ST7789 PANEL ***
    //esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .bits_per_pixel = 16,
    };

    ESP_LOGI("INIT", "Initializing ST7789 PANEL...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    // Send custom MADCTL to fix RBG channel order and the display orientation
    esp_lcd_panel_io_tx_param(io_handle, 0x36, (uint8_t[]){0x60}, 1);
    esp_lcd_panel_set_gap(panel_handle, LCD_X_OFFSET, LCD_Y_OFFSET);    // 82, 18 for 76x284, 0, 0 for 240x240
    esp_lcd_panel_disp_on_off(panel_handle, true);

    ESP_LOGI("INIT", "Done with display initialization, starting LVGL...");
    lv_init();

    /*** LVGL BUFFER ***/
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[LCD_H_RES * LCD_V_RES];
    static lv_color_t buf2[LCD_H_RES * LCD_V_RES];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LCD_V_RES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = panel_handle; // Pass the panel handle to the driver
    lv_disp_drv_register(&disp_drv);    

    // *** BACKLIGHT ***
    gpio_set_direction(PIN_NUM_LCD_BCKL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_LCD_BCKL, 0); // Backlight on for ST7789P3

    const esp_timer_create_args_t tick_timer_args = {
        .callback = &lv_tick_task,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_timer_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 10 * 1000); // 10ms in microseconds

    lv_obj_t *Screen1 = lv_obj_create(NULL); // Create a screen object
    lv_obj_set_style_bg_color(Screen1, lv_color_black(), LV_PART_MAIN); // Set background color to black
    ProtocolLabel = lv_label_create(Screen1);
    lv_obj_set_width(ProtocolLabel, 130);
    lv_obj_set_height(ProtocolLabel, 12);
    lv_label_set_text(ProtocolLabel, "System Status");
    lv_obj_set_style_text_color(ProtocolLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ProtocolLabel, &lv_font_montserrat_12, LV_PART_MAIN);

    HostLabel = lv_label_create(Screen1);
    lv_obj_set_height(HostLabel, 30);
    lv_obj_set_width(HostLabel, lv_pct(100));
    lv_obj_set_x(HostLabel, 0);
    lv_obj_set_y(HostLabel, -10);
    lv_obj_set_align(HostLabel, LV_ALIGN_LEFT_MID);
    lv_label_set_long_mode(HostLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    lv_label_set_text(HostLabel, "ESP32-S3 ST7789");
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    lv_label_set_text(HostLabel, "ESP32-P4 ST7789");
#elif defined(CONFIG_IDF_TARGET_ESP32)
    lv_label_set_text(HostLabel, "ESP32 ST7789");
#endif
    lv_obj_set_style_text_color(HostLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_anim_speed(HostLabel, 200, LV_PART_MAIN);

    InterfaceLabel = lv_label_create(Screen1);
    lv_obj_set_height(InterfaceLabel, 30);
    lv_obj_set_width(InterfaceLabel, lv_pct(100));
    lv_obj_set_x(InterfaceLabel, 0);
    lv_obj_set_y(InterfaceLabel, 0);
    lv_obj_set_align(InterfaceLabel, LV_ALIGN_BOTTOM_LEFT);
    lv_label_set_long_mode(InterfaceLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(InterfaceLabel, "Version 1.2");
    lv_obj_set_style_text_color(InterfaceLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_anim_speed(InterfaceLabel, 200, LV_PART_MAIN);

    VlanLabel = lv_label_create(Screen1);
    lv_obj_set_height(VlanLabel, 12);
    lv_obj_set_width(VlanLabel, 35);
    lv_obj_set_x(VlanLabel, -14);
    lv_obj_set_y(VlanLabel, 0);
    lv_obj_set_align(VlanLabel, LV_ALIGN_TOP_RIGHT);
    lv_label_set_text(VlanLabel, "VLAN");
    lv_obj_set_style_text_align(VlanLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(VlanLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(VlanLabel, &lv_font_montserrat_12, LV_PART_MAIN);

#if defined(USE_CIRCLE_INDICATOR)
    SpeedIndicator = lv_obj_create(Screen1);
    lv_obj_set_style_radius(SpeedIndicator, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_align(SpeedIndicator, LV_ALIGN_CENTER);
    lv_obj_set_x(SpeedIndicator, 136);
    lv_obj_set_y(SpeedIndicator, -32);
    lv_obj_set_style_bg_color(SpeedIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_border_width(SpeedIndicator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(SpeedIndicator, 0, LV_PART_MAIN);
    lv_obj_set_style_size(SpeedIndicator, 12, LV_PART_MAIN);
#else
    lv_obj_set_pos(SpeedIndicator, 0, 0);
    SpeedIndicator = lv_bar_create(Screen1);
    lv_obj_set_height(SpeedIndicator, 12); 
    lv_obj_set_width(SpeedIndicator, 12);
    lv_bar_set_range(SpeedIndicator, 0, 200); // Set the range from 0 to 200
    lv_bar_set_value(SpeedIndicator, 200, LV_ANIM_OFF); // Set initial value to 200 (200 ms)
    lv_obj_set_align(SpeedIndicator, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(SpeedIndicator, 0);
    lv_obj_set_y(SpeedIndicator, 0); 
    lv_obj_set_style_border_width(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(SpeedIndicator, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(SpeedIndicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_base_dir(SpeedIndicator, LV_BASE_DIR_RTL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(SpeedIndicator, lv_color_hex(0xFF0000), LV_PART_INDICATOR | LV_STATE_DEFAULT); // Red color for the indicator
    lv_obj_set_style_bg_color(SpeedIndicator, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT); // Black for the background
#endif

    PlatformLabel = lv_label_create(Screen1);
    lv_obj_set_height(PlatformLabel, 12);
    lv_obj_set_width(PlatformLabel, 180);
    lv_obj_set_x(PlatformLabel, -51);
    lv_obj_set_y(PlatformLabel, 0);
    lv_obj_set_align(PlatformLabel, LV_ALIGN_TOP_RIGHT);
    lv_label_set_text(PlatformLabel, "Model");
    lv_obj_set_style_text_align(PlatformLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(PlatformLabel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(PlatformLabel, &lv_font_montserrat_12, LV_PART_MAIN);

    // Add a mutex to protect LVGL calls since we'll be updating from multiple tasks
    lvgl_mutex = xSemaphoreCreateMutex();
    packet_queue = xQueueCreate(1, sizeof(parsed_packet_t));

    // Load the Default LVGL screen
    lv_scr_load(Screen1);

    // GPIO for scroll speed control before starting GPIO task
	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_NEGEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_SCRLSPD),
        .pull_down_en = 0,
        .pull_up_en   = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);
	gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
	gpio_isr_handler_add(PIN_NUM_SCRLSPD, gpio_isr_handler, (void *)PIN_NUM_SCRLSPD);
    // Start the display update task
    xTaskCreate(display_update_task, "display_update_task", 4096, NULL, 3, NULL);
    // Start the GPIO scroll speed task
   	xTaskCreate(gpio_scroll_speed_task, "gpio_scroll_speed_task", 4096, NULL, 4, NULL);

    // Start Ethernet capture Required by esp_eth_start() even without TCP/IP stack
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    eth_capture_init();

    while (1) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lv_timer_handler(); // This updates the screen
            xSemaphoreGive(lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}