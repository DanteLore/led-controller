#include "captive_dns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>

#define DNS_PORT     53
#define HEADER_LEN   12
#define ANSWER_LEN   16
#define TTL_SECONDS  10
#define TYPE_A        1
#define TYPE_PTR     12
#define NXDOMAIN      3

static const char *TAG = "captive_dns";

static volatile bool enabled;
static in_addr_t address;

// Shows which names a phone looks up, to see how it probes for a captive portal
static void log_question(const uint8_t *p, int name_end, int type)
{
    char name[128];
    int n = 0;
    for (int i = HEADER_LEN; i < name_end - 1 && n < (int)sizeof(name) - 1; ) {
        int label = p[i++];
        if (n > 0)
            name[n++] = '.';
        for (int j = 0; j < label && i < name_end && n < (int)sizeof(name) - 1; j++)
            name[n++] = p[i++];
    }
    name[n] = 0;
    ESP_LOGI(TAG, "Query %s (type %d)", name, type);
}

// Letters, digits and hyphens: names that could belong to a real site. Some
// phones test for DNS spoofing by looking up impossible names (*google.com,
// google.com.onion) and distrust a network that answers them.
static bool hostname_label(const uint8_t *label, int len)
{
    for (int i = 0; i < len; i++)
        if (!isalnum(label[i]) && label[i] != '-')
            return false;
    return len > 0 && !(len == 5 && strncasecmp((const char *)label, "onion", 5) == 0);
}

// Turns a query into its reply in place and returns the reply's length, or 0
// to ignore it. Only the first question is answered. A queries for plausible
// names get our address; impossible names and reverse lookups get "no such
// name"; anything else (AAAA, say) an empty answer, so phones don't wait.
static int build_reply(uint8_t *p, int len)
{
    if (len < HEADER_LEN)
        return 0;
    bool is_query = !(p[2] & 0x80) && ((p[2] >> 3) & 0x0F) == 0;
    bool one_question = p[4] == 0 && p[5] == 1;
    if (!is_query || !one_question)
        return 0;

    // Question: a name as length-prefixed labels ending in 0, then type and class
    int end = HEADER_LEN;
    bool plausible = true;
    while (end < len && p[end] != 0) {
        int label = p[end];
        if (label & 0xC0 || end + 1 + label > len)
            return 0;
        plausible = plausible && hostname_label(&p[end + 1], label);
        end += label + 1;
    }
    end += 5;
    if (end > len)
        return 0;
    int type = (p[end - 4] << 8) | p[end - 3];
    log_question(p, end - 4, type);
    bool no_such_name = !plausible || type == TYPE_PTR;
    bool type_a = type == TYPE_A && !no_such_name;

    p[2] = 0x84 | (p[2] & 0x01);           // reply, authoritative, echo recursion desired
    p[3] = 0x80 | (no_such_name ? NXDOMAIN : 0);   // recursion available
    p[6] = 0;
    p[7] = type_a;                         // answer count
    memset(&p[8], 0, 4);           // drop authority and additional records
    if (!type_a)
        return end;

    const uint8_t *ip = (const uint8_t *)&address;
    const uint8_t answer[ANSWER_LEN] = {
        0xC0, HEADER_LEN,          // name: pointer back to the question's
        0, TYPE_A, 0, 1,           // type A, class IN
        0, 0, 0, TTL_SECONDS,
        0, 4, ip[0], ip[1], ip[2], ip[3],
    };
    memcpy(&p[end], answer, ANSWER_LEN);
    return end + ANSWER_LEN;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "Can't bind port %d", DNS_PORT);
        vTaskDelete(NULL);
    }

    uint8_t packet[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        // Leaves room to append the answer
        int len = recvfrom(sock, packet, sizeof(packet) - ANSWER_LEN, 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0 || !enabled)
            continue;
        int reply_len = build_reply(packet, len);
        if (reply_len > 0)
            sendto(sock, packet, reply_len, 0, (struct sockaddr *)&from, from_len);
    }
}

void captive_dns_start(const char *ip)
{
    address = inet_addr(ip);
    xTaskCreatePinnedToCore(dns_task, "captive_dns", 3072, NULL, 5, NULL, 0);
}

void captive_dns_enable(bool on)
{
    enabled = on;
}
