#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_log.h"
#include "lwip/dns.h"
#include "esp_wifi.h"

static const char *TAG = "DNS_SERVER";
static int s_dns_socket = -1;

// Minimal DNS header structure
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int err = bind(s_dns_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(s_dns_socket);
        s_dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Server started on port 53");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        if (len > sizeof(dns_header_t)) {
            dns_header_t *header = (dns_header_t *)rx_buffer;
            
            // Standard query response, no error
            // QR=1, Opcode=0, AA=1, TC=0, RD=1, RA=1, Z=0, RCODE=0
            header->flags = htons(0x8180); 
            header->an_count = header->qd_count;
            header->ar_count = 0;
            header->ns_count = 0;

            // Simple DNS Parsing to find end of QNAME
            // We just skip the QNAME and QTYPE/QCLASS to append answer
            int idx = sizeof(dns_header_t);
            
            // Skip QNAME
            while (idx < len && rx_buffer[idx] != 0) {
                idx += rx_buffer[idx] + 1;
            }
            idx += 1; // End of domain
            idx += 4; // QTYPE + QCLASS

            // Only append answer if buffer has space
            // Answer is 16 bytes fixed
            if (idx + 16 <= sizeof(rx_buffer)) {
                // Name ptr (pointer to offset 12 - the start of QNAME)
                rx_buffer[idx++] = 0xC0;
                rx_buffer[idx++] = 0x0C; 
                
                // Type A (0x0001)
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x01;

                // Class IN (0x0001)
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x01;

                // TTL (60s)
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x3C;

                // Data Length (4)
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x04;

                // IP Address: 192.168.4.1 (C0 A8 04 01)
                rx_buffer[idx++] = 192;
                rx_buffer[idx++] = 168;
                rx_buffer[idx++] = 4;
                rx_buffer[idx++] = 1;

                sendto(s_dns_socket, rx_buffer, idx, 0, (struct sockaddr *)&source_addr, socklen);
                // ESP_LOGD(TAG, "DNS query replied");
            }
        }
    }

    if (s_dns_socket != -1) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    vTaskDelete(NULL);
}

void start_dns_server(void) {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

void stop_dns_server(void) {
    if (s_dns_socket != -1) {
        close(s_dns_socket); // This should cause recvfrom to fail and task to exit
        s_dns_socket = -1;
    }
}
