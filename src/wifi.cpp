#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_task_wdt.h>

#include "oled.h"
#include "power_management.h"

static const char LOG_TAG[] = __FILE__;

static SemaphoreHandle_t mutex;

static bool is_powered_on = false;

static unsigned long round_num = 0;
static size_t channel_num = 1;
static size_t pkt_counter = 0;
static size_t channel_pkt_counter[WIFI_MAX_CHANNEL_NUM];
static const wifi_promiscuous_filter_t pkt_filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
static wifi_country_t wifi_country_params = {"EU", 1, WIFI_MAX_CHANNEL_NUM, 100, WIFI_COUNTRY_POLICY_MANUAL};

static uint8_t last_loudest_sender[6], loudest_sender[6];
static int last_loudest_rssi = WIFI_RSSI_FLOOR, loudest_rssi = WIFI_RSSI_FLOOR;
static size_t last_loudest_channel = 0, loudest_channel = 0;

void wifi_setup()
{
    memset(&channel_pkt_counter, 0, sizeof(channel_pkt_counter));
    mutex = xSemaphoreCreateMutex();
    wifi_on();
}

void wifi_on()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (is_powered_on)
    {
        xSemaphoreGive(mutex);
        return;
    }
    ESP_LOGI(LOG_TAG, "turing on WiFi");
    wifi_init_config_t wifi_init_conf = WIFI_INIT_CONFIG_DEFAULT();
    wifi_init_conf.nvs_enable = 0;
    // Core 1 is already occupied by a great number of tasks, see setup.
    wifi_init_conf.wifi_task_core_id = 0;
    esp_wifi_init(&wifi_init_conf);
    esp_wifi_set_country(&wifi_country_params);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_wifi_start();
    esp_wifi_set_channel(channel_num, WIFI_SECOND_CHAN_NONE);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_filter(&pkt_filter);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    is_powered_on = true;
    xSemaphoreGive(mutex);
}

void wifi_off()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!is_powered_on)
    {
        xSemaphoreGive(mutex);
        return;
    }
    ESP_LOGI(LOG_TAG, "turing off WiFi");
    esp_wifi_disconnect();
    esp_wifi_scan_stop();
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    is_powered_on = false;
    xSemaphoreGive(mutex);
}

void wifi_task_loop(void *_)
{
    while (true)
    {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WIFI_TASK_LOOP_DELAY_MS));
        if ((power_get_todo() & POWER_TODO_WARMING_UP_FOR_TX) || (oled_is_awake() && oled_get_page_number() == OLED_PAGE_WIFI_INFO))
        {
            wifi_on();
            wifi_next_channel();
        }
        else
        {
            wifi_off();
        }
    }
}

void wifi_next_channel()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    channel_pkt_counter[channel_num - 1] = pkt_counter;
    pkt_counter = 0;
    if (++channel_num > WIFI_MAX_CHANNEL_NUM)
    {
        channel_num = 1;
        round_num++;
        // Remember the loudest sender from this round.
        last_loudest_rssi = loudest_rssi;
        last_loudest_channel = loudest_channel;
        memcpy(last_loudest_sender, loudest_sender, sizeof(uint8_t) * 6);
        // Reset the stats for the new round.
        loudest_rssi = WIFI_RSSI_FLOOR;
        loudest_channel = 0;
        memset(loudest_sender, 0, sizeof(loudest_sender));
    }
    esp_wifi_set_channel(channel_num, WIFI_SECOND_CHAN_NONE);
    channel_pkt_counter[channel_num] = 0;
    xSemaphoreGive(mutex);
}

int wifi_get_last_loudest_sender_rssi()
{
    return last_loudest_rssi;
}

uint8_t *wifi_get_last_loudest_sender_mac()
{
    return last_loudest_sender;
}

size_t wifi_get_last_loudest_sender_channel()
{
    return last_loudest_channel;
}

size_t wifi_get_total_num_pkts()
{
    size_t sum = 0;
    for (size_t i = 0; i < WIFI_MAX_CHANNEL_NUM; ++i)
    {
        sum += channel_pkt_counter[i];
    }
    return sum;
}

size_t wifi_get_channel_num()
{
    return channel_num;
}

unsigned long wifi_get_round_num()
{
    return round_num;
}

void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT)
        return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buff;
    const wifi_ieee80211_packet_t *payload = (wifi_ieee80211_packet_t *)pkt->payload;
    const wifi_ieee80211_mac_hdr_t *header = &payload->hdr;
    pkt_counter++;
    if (pkt->rx_ctrl.rssi > loudest_rssi)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        loudest_rssi = pkt->rx_ctrl.rssi;
        loudest_channel = pkt->rx_ctrl.channel;
        memcpy(loudest_sender, header->addr2, sizeof(uint8_t) * 6);
        xSemaphoreGive(mutex);
    }
}