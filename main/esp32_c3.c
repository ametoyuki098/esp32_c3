#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

// ================== 纯 UART 配置 ==================
#define SU03T_UART_NUM      UART_NUM_1
#define SU03T_TX_PIN        0  // ESP32-C3 的 GPIO0 (板子引脚 2)
#define SU03T_RX_PIN        1  // ESP32-C3 的 GPIO1 (板子引脚 3)

void uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,                // 电脑测试成功是 9600，先用 9600
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(SU03T_UART_NUM, 1024, 1024, 0, NULL, 0);
    uart_param_config(SU03T_UART_NUM, &uart_config);
    uart_set_pin(SU03T_UART_NUM, SU03T_TX_PIN, SU03T_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// ================== 主程序（纯监听） ==================
void app_main(void) {
    uart_init();
    printf("纯净版监听启动，波特率 9600，等待语音模块数据...\n");

    uint8_t data[128];
    while (1) {
        // 读取数据，超时 100ms
        int len = uart_read_bytes(SU03T_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(100));
        
        if (len > 0) {
            printf("\n>>> 收到 %d 字节数据 <<<\n", len);
            // 打印十六进制
            ESP_LOG_BUFFER_HEX("RAW_RX", data, len);
            // 打印字符串形式
            data[len] = '\0';
            printf("字符串内容: %s\n", data);
        }
    }
}
