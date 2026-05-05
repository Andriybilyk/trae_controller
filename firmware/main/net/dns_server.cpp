#include "net/dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_log.h"

static const char *TAG = "DNS_SERVER";
static int s_dns_socket = -1;
static TaskHandle_t s_dns_task = nullptr;

static constexpr int DNS_PORT = 53;
static constexpr int DNS_BUF_SIZE = 512;

static int dns_question_end_offset(const uint8_t *buf, int len) {
    if (!buf || len < 12) return -1;
    int off = 12;
    while (off < len) {
        const uint8_t label_len = buf[off];
        if (label_len == 0) {
            off += 1;
            break;
        }
        // Reject compression pointers in question for this tiny captive DNS responder.
        if ((label_len & 0xC0) == 0xC0) return -1;
        off += 1 + label_len;
    }
    // Need QTYPE + QCLASS
    if (off + 4 > len) return -1;
    return off + 4;
}

// Matches MichMich/esp-idf-wifi-provisioner DNS behavior:
// answer every DNS question with A=192.168.4.1 to force captive portal detection.
static void dns_server_task(void *pvParameters) {
    (void)pvParameters;
    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client = {};
    socklen_t client_len = sizeof(client);

    const uint32_t ap_ip = htonl(0xC0A80401); // 192.168.4.1

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        s_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_socket, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port %d", DNS_PORT);

    while (true) {
        client_len = sizeof(client);
        int len = recvfrom(s_dns_socket, buf, sizeof(buf), 0,
                           reinterpret_cast<struct sockaddr *>(&client), &client_len);
        if (len < 0) {
            break; // socket closed by stop_dns_server()
        }
        if (len < 12) {
            continue; // too short for DNS header
        }

        // Build response in-place.
        buf[2] = 0x81; // QR=1, Opcode=0, AA=1
        buf[3] = 0x80; // RA=1, RCODE=0 (No error)
        // Keep one question
        buf[4] = 0x00;
        buf[5] = 0x01;
        // Answer count = 1
        buf[6] = 0x00;
        buf[7] = 0x01;
        // Authority/additional = 0
        buf[8] = 0x00;
        buf[9] = 0x00;
        buf[10] = 0x00;
        buf[11] = 0x00;

        // Append one A answer right after the DNS question section.
        const int q_end = dns_question_end_offset(buf, len);
        if (q_end < 0 || q_end >= DNS_BUF_SIZE - 16) {
            continue;
        }
        uint8_t *p = buf + q_end;
        *p++ = 0xC0; *p++ = 0x0C; // name pointer to question at offset 12
        *p++ = 0x00; *p++ = 0x01; // Type A
        *p++ = 0x00; *p++ = 0x01; // Class IN
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C; // TTL 60s
        *p++ = 0x00; *p++ = 0x04; // RDLENGTH 4
        memcpy(p, &ap_ip, 4);
        p += 4;

        sendto(s_dns_socket, buf, static_cast<size_t>(p - buf), 0,
               reinterpret_cast<struct sockaddr *>(&client), client_len);
    }

    ESP_LOGI(TAG, "DNS server stopped");
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    s_dns_task = nullptr;
    vTaskDelete(nullptr);
}

void start_dns_server(void) {
    if (s_dns_task != nullptr) return;
    xTaskCreate(dns_server_task, "dns_server", 4096, nullptr, 5, &s_dns_task);
}

void stop_dns_server(void) {
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, SHUT_RDWR);
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    s_dns_task = nullptr;
}
