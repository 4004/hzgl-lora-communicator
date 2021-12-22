#include <Arduino.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <lmic.h>
#include <hal/hal.h>
#include "data_packet.h"
#include "env_sensor.h"
#include "gp_button.h"
#include "gps.h"
#include "hardware_facts.h"
#include "i2c.h"
#include "lorawan.h"
#include "wifi.h"
#include "bluetooth.h"
#include "lorawan_creds.h"
#include "oled.h"
#include "power_management.h"

static const char LOG_TAG[] = __FILE__;

const lmic_pinmap lmic_pins = {
    .nss = SPI_NSS_GPIO,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = LORA_RST_GPIO,
    .dio = {LORA_DIO0_GPIO, LORA_DIO1_GPIO, LORA_DIO2_GPIO},
};

static size_t total_tx_bytes = 0, total_rx_bytes = 0;
static lorawan_message_buf_t next_tx_message, last_rx_message;
static lorawan_power_config_t power_config = lorawan_power_regular;
static unsigned long last_transmision_timestamp = 0, tx_counter = 0;

// os_getArtEui is referenced by "engineUpdate" symbol defined by the "MCCI LoRaWAN LMIC" library.
void os_getArtEui(u1_t *buf) {}
// os_getDevEui is referenced by "engineUpdate" symbol defined by the "MCCI LoRaWAN LMIC" library.
void os_getDevEui(u1_t *buf) {}
// os_getDevKey is referenced by "engineUpdate" symbol defined by the "MCCI LoRaWAN LMIC" library.
void os_getDevKey(u1_t *buf) {}

void lorawan_handle_message(uint8_t message)
{
  switch (message)
  {
  case EV_JOINING:
    ESP_LOGI(LOG_TAG, "joining network");
    break;
  case EV_JOINED:
    ESP_LOGI(LOG_TAG, "joined network");
    break;
  case EV_JOIN_FAILED:
    ESP_LOGI(LOG_TAG, "failed to join network");
    break;
  case EV_REJOIN_FAILED:
    ESP_LOGI(LOG_TAG, "failed to rejoin network");
    break;
  case EV_RESET:
    ESP_LOGI(LOG_TAG, "reset network connection");
    break;
  case EV_LINK_DEAD:
    // This is only applicable when adaptive-data-rate (see LMIC_setAdrMode) is enabled.
    ESP_LOGI(LOG_TAG, "network link is dead");
    break;
  case LORAWAN_EV_ACK:
    ESP_LOGI(LOG_TAG, "my transmitted message was acknowledged");
    break;
  case LORAWAN_EV_IDLING_BEFORE_TXRX:
    ESP_LOGI(LOG_TAG, "idling before upcoming TX/RX");
    break;
  case LORAWAN_EV_QUEUED_FOR_TX:
    ESP_LOGI(LOG_TAG, "queued a %d bytes message for transmission", next_tx_message.len);
    break;
  case EV_TXCOMPLETE:
    ESP_LOGI(LOG_TAG, "transmitted a %d bytes message", next_tx_message.len);
    break;
  case EV_RXCOMPLETE:
    ESP_LOGI(LOG_TAG, "received a message");
    break;
  case LORAWAN_EV_RESPONSE:
    // I've managed to receive a downlink message 41 bytes long maximum.
    ESP_LOGI(LOG_TAG, "received a %d bytes downlink message", LMIC.dataLen);
    total_rx_bytes += LMIC.dataLen;
    size_t data_len = LMIC.dataLen;
    if (data_len > 0)
    {
      if (data_len > LORAWAN_MAX_MESSAGE_LEN)
      {
        data_len = LORAWAN_MAX_MESSAGE_LEN;
      }
      for (uint8_t i = 0; i < data_len; i++)
      {
        last_rx_message.buf[i] = LMIC.frame[LMIC.dataBeg + i];
      }
      last_rx_message.len = data_len;
      last_rx_message.buf[last_rx_message.len] = 0;
      last_rx_message.timestamp_millis = millis();
    }
    break;
  }
}

// onEvent is referenced by MCCI LMIC library.
void onEvent(ev_t event)
{
  switch (event)
  {
  case EV_TXCOMPLETE:
    next_tx_message.timestamp_millis = millis();
    tx_counter++;
    ESP_LOGI(LOG_TAG, "finished transmitting a %d bytes message, tx counter is now %d", next_tx_message.len, tx_counter);
    if (LMIC.txrxFlags & TXRX_ACK)
    {
      ESP_LOGI(LOG_TAG, "received an acknowledgement of my transmitted message");
      lorawan_handle_message(LORAWAN_EV_ACK);
    }
    if (LMIC.dataLen > 0)
    {
      ESP_LOGI(LOG_TAG, "received a downlink message");
      lorawan_handle_message(LORAWAN_EV_RESPONSE);
    }
    break;
  case EV_TXSTART:
    ESP_LOGI(LOG_TAG, "start transmitting a %d bytes message", next_tx_message.len);
    break;
  default:
    ESP_LOGI(LOG_TAG, "ignored unrecognised event %d", event);
    break;
  }
  lorawan_handle_message(event);
}

void lorawan_setup()
{
  SPI.begin(SPI_SCK_GPIO, SPI_MISO_GPIO, SPI_MOSI_GPIO, SPI_NSS_GPIO);
  memset(&last_rx_message, 0, sizeof(last_rx_message));
  memset(&next_tx_message, 0, sizeof(next_tx_message));

  // Initialise the library's internal states.
  os_init();
  LMIC_reset();

  // Prepare network keys for the library to use. They are defined in #include "lorawan_creds.h":
  // static const u1_t PROGMEM NWKSKEY[16] = {0x00, 0x00, ...};
  // static const u1_t PROGMEM APPSKEY[16] = {0x00, 0x00, ...};
  // static const u4_t DEVADDR = 0x00000000;
  uint8_t appskey[sizeof(APPSKEY)];
  uint8_t nwkskey[sizeof(NWKSKEY)];
  memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
  memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
  LMIC_setSession(0x1, DEVADDR, nwkskey, appskey);

  // The Things Stack Community Edition could potentially use all of these channels.
  LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);
  LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  // Though I am unsure if The Things Stack Community Edition uses FSK.
  LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK, DR_FSK), BAND_MILLI);
  // This is in the frequency plan - "Europe 863-870 MHz (SF9 for RX2 - recommended)".
  LMIC.dn2Dr = DR_SF9;

  // Do not ask gateways for a downlink message to check the connectivity.
  LMIC_setLinkCheckMode(0);

  // Do not lower transmission power automatically. According to The Things Network this feature is tricky to use.
  LMIC_setAdrMode(0);
  // Open up the RX window earlier ("clock error to compensate for").
  LMIC_setClockError(MAX_CLOCK_ERROR * 12 / 100);

  // The transmitter is activated by personalisation (i.e. static keys), so it has already "joined" the network.
  lorawan_handle_message(EV_JOINED);
  ESP_LOGI(LOG_TAG, "successfully initialised LoRaWAN");
}

void lorawan_set_next_transmission(uint8_t *buf, size_t len, int port)
{
  if (len > LORAWAN_MAX_MESSAGE_LEN)
  {
    len = LORAWAN_MAX_MESSAGE_LEN;
  }
  memcpy(next_tx_message.buf, buf, len);
  next_tx_message.len = len;
  next_tx_message.port = port;
}

lorawan_message_buf_t lorawan_get_last_reception()
{
  return last_rx_message;
}

lorawan_message_buf_t lorawan_get_transmission()
{
  return next_tx_message;
}

size_t lorawan_get_total_tx_bytes()
{
  return total_tx_bytes;
}

size_t lorawan_get_total_rx_bytes()
{
  return total_rx_bytes;
}

void lorawan_prepare_uplink_transmission()
{
  int message_kind = tx_counter % 3;
  if (message_kind == 0)
  {
    DataPacket pkt(LORAWAN_MAX_MESSAGE_LEN);
    // Byte 0, 1 - number of seconds since the reception of last downlink message (0 - 65535).
    lorawan_message_buf_t last_reception = lorawan_get_last_reception();
    unsigned long last_rx = (millis() - last_reception.timestamp_millis) / 1000;
    if (last_rx > 65536 || last_rx == 0)
    {
      last_rx = 65536;
    }
    pkt.writeInteger(last_rx, 2);
    // Byte 2, 3, 4, 5 - uptime in seconds.
    pkt.writeInteger(power_get_uptime_sec(), 4);
    // Byte 6, 7 - heap usage in KB.
    pkt.writeInteger((ESP.getHeapSize() - ESP.getFreeHeap()) / 1024, 2);
    // Byte 8, 9 - battery voltage in millivolts.
    struct power_status power = power_get_status();
    pkt.writeInteger(power.batt_millivolt, 2);
    // Byte 10, 11 - power supply current draw in milliamps.
    pkt.writeInteger((int)power.power_draw_milliamp, 2);
    // Byte 12 - is battery charging (0 - false, 1 - true).
    pkt.writeInteger(power.is_batt_charging ? 1 : 0, 1);
    // Byte 13, 14, 15, 16 - ambient temperature in celcius.
    struct env_data env = env_sensor_get_data();
    pkt.write32BitDouble(env.temp_celcius);
    // Byte 17 - ambient humidity in percentage.
    pkt.writeInteger((int)env.humidity_pct, 1);
    // Byte 18, 19, 20, 21 - ambient pressure in hpa.
    pkt.write32BitDouble(env.pressure_hpa);
    // Byte 22, 23, 24, 25 - pressure altitude in meters.
    pkt.write32BitDouble(env.altitude_metre);
    lorawan_set_next_transmission(pkt.content, pkt.cursor, LORAWAN_PORT_STATUS_SENSOR);
    ESP_LOGI(LOG_TAG, "going to transmit status and sensor info in %d bytes", pkt.cursor);
  }
  else if (message_kind == 1)
  {
    DataPacket pkt(LORAWAN_MAX_MESSAGE_LEN);
    // Byte 0, 1, 2, 3 - GPS latitude.
    struct gps_data gps = gps_get_data();
    pkt.write32BitDouble(gps.latitude);
    // Byte 4, 5, 6, 7 - GPS longitude.
    pkt.write32BitDouble(gps.longitude);
    // Byte 8, 9 - GPS speed in km/h.
    pkt.writeInteger((int)gps.speed_kmh, 2);
    // Byte 10, 11 - GPS heading in degrees.
    pkt.writeInteger((int)gps.heading_deg, 2);
    // Byte 12, 13, 14, 15 - GPS altitude in metres.
    pkt.write32BitDouble(gps.altitude_metre);
    // Byte 16, 17 - the age of last GPS fix in seconds (0 - 65536).
    if (gps.pos_age_sec > 65536)
    {
      gps.pos_age_sec = 65536;
    }
    pkt.writeInteger(gps.pos_age_sec, 2);
    // Byte 18 - HDOP in integer (0 - 256).
    int hdop = (int)gps.hdop;
    if (hdop > 256)
    {
      hdop = 256;
    }
    pkt.writeInteger(hdop, 1);
    // Byte 19 - number of GPS satellites in view.
    pkt.writeInteger(gps.satellites, 1);
    // Byte 20 - WiFi monitor - number of inflight packets across all channels.
    pkt.writeInteger(wifi_get_total_num_pkts(), 1);
    // Byte 21 - WiFi monitor - the loudest sender's channel.
    pkt.writeInteger(wifi_get_last_loudest_sender_channel(), 1);
    // Byte 22 - WiFi monitor - the loudest sender's RSSI reading above RSSI floor (which is -100).
    int wifi_rssi = wifi_get_last_loudest_sender_rssi();
    if (wifi_rssi < -100)
    {
      wifi_rssi = -100;
    }
    pkt.writeInteger(wifi_rssi - WIFI_RSSI_FLOOR, 1);
    // Byte 23, 24, 25, 26, 27, 28 - WiFi monitor - the loudest sender's MAC address.
    uint8_t *wifi_mac = wifi_get_last_loudest_sender_mac();
    for (int i = 0; i < 6; ++i)
    {
      pkt.writeInteger(wifi_mac[i], 1);
    }
    // Byte 29 - Bluetooth monitor - number of devices in the vicinity.
    pkt.writeInteger(bluetooth_get_total_num_devices(), 1);
    // Byte 30 - Bluetooth monitor - the loudest sender's RSSI reading above RSSI floor (which is -100).
    BLEAdvertisedDevice dev = bluetooth_get_loudest_sender();
    int bt_rssi = dev.getRSSI();
    if (bt_rssi < -100)
    {
      bt_rssi = -100;
    }
    pkt.writeInteger(bt_rssi - BLUETOOTH_RSSI_FLOOR, 1);
    // Byte 31, 32, 33, 34, 35, 36 - Bluetooth monitor - the loudest sender's MAC address.
    uint8_t bt_mac[6];
    memset(bt_mac, 0, sizeof(bt_mac));
    if (dev.haveRSSI())
    {
      memcpy(bt_mac, dev.getAddress().getNative(), 6);
    }
    for (int i = 0; i < 6; ++i)
    {
      pkt.writeInteger(bt_mac[i], 1);
    }
    lorawan_set_next_transmission(pkt.content, pkt.cursor, LORAWAN_PORT_GPS_WIFI);
    ESP_LOGI(LOG_TAG, "going to transmit GPS, wifi, and bluetooth info in %d bytes", pkt.cursor);
  }
  else if (message_kind == 2)
  {
    String morse_message = gp_button_get_morse_message_buf();
    uint8_t buf[LORAWAN_MAX_MESSAGE_LEN] = {0};
    for (int i = 0; i < morse_message.length(); ++i)
    {
      buf[i] = (uint8_t)morse_message.charAt(i);
    }
    // Determine the type of the message according to which OLED page the input came from.
    int port = LORAWAN_PORT_MESSAGE;
    if (oled_get_last_morse_input_page_num() == OLED_PAGE_TX_COMMAND)
    {
      port = LORAWAN_PORT_COMMAND;
    }
    // Set the transmission buffer only if user is has finished typing a message.
    if (oled_get_page_number() != OLED_PAGE_TX_COMMAND && oled_get_page_number() != OLED_PAGE_TX_MESSAGE)
    {
      lorawan_set_next_transmission(buf, morse_message.length(), port);
      ESP_LOGI(LOG_TAG, "going to transmit message/command \"%s\"", morse_message.c_str());
    }
  }
}

void lorawan_debug_to_log()
{
  ESP_LOGI(LOG_TAG, "LORAWANDEBUG: os_getTime - %d ticks = %d sec", os_getTime(), osticks2ms(os_getTime()) / 1000);
  ESP_LOGI(LOG_TAG, "LORAWANDEBUG: globalDutyRate - %d ticks = %d sec", LMIC.globalDutyRate, osticks2ms(LMIC.globalDutyRate) / 1000);
  ESP_LOGI(LOG_TAG, "LORAWANDEBUG: LMICbandplan_nextTx - %d ticks = %d sec", LMICbandplan_nextTx(os_getTime()), osticks2ms(LMICbandplan_nextTx(os_getTime())) / 1000);
  ESP_LOGI(LOG_TAG, "LORAWANDEBUG: txend - %d, txChnl - %d", LMIC.txend, LMIC.txChnl);
  for (size_t band = 0; band < MAX_BANDS; ++band)
  {
    ESP_LOGI(LOG_TAG, "LORAWANDEBUG \"band\"[%d] - next avail at %d sec, lastchnl %d, txpow %d, txcap %d", band, osticks2ms(LMIC.bands[band].avail) / 1000, LMIC.bands[band].lastchnl, LMIC.bands[band].txpow, LMIC.bands[band].txcap);
  }
}

void lorawan_reset_tx_stats()
{
  // Rely on LORAWAN_TX_INTERVAL_MS alone to control the duty cycle. Reset LMIC library's internal duty cycle stats.
  for (size_t band = 0; band < MAX_BANDS; ++band)
  {
    LMIC.bands[band].avail = ms2osticks(millis() - LORAWAN_TASK_LOOP_DELAY_MS * 2);
    if (LMIC.bands[band].avail < 0)
    {
      LMIC.bands[band].avail = 0;
    }
    LMIC.bands[band].txpow = power_config.power_dbm;
  }
}

void lorawan_transceive()
{
  // Give the LoRaWAN library a chance to do its work.
  os_runloop_once();
  // Rate-limit transmission to observe duty cycle.
  if (last_transmision_timestamp == 0 || millis() - last_transmision_timestamp > power_config.tx_internal_sec * 1000)
  {
    lorawan_prepare_uplink_transmission();
    last_transmision_timestamp = millis();
    // Reset transmission power and spreading factor.
    LMIC_setDrTxpow(power_config.spreading_factor, power_config.power_dbm);
    lorawan_reset_tx_stats();
    lmic_tx_error_t err = LMIC_setTxData2_strict(next_tx_message.port, next_tx_message.buf, next_tx_message.len, false);
    lorawan_reset_tx_stats();
    // lorawan_debug_to_log();
    if (err == LMIC_ERROR_SUCCESS)
    {
      total_tx_bytes += next_tx_message.len;
    }
    else
    {
      ESP_LOGI(LOG_TAG, "failed to transmit LoRaWAN message due to error code %d", err);
      lorawan_debug_to_log();
    }
    if (LMIC.opmode & OP_TXRXPEND)
    {
      // My gut feeling says this state exists simply for avoiding to exceed duty cycle limit.
      lorawan_handle_message(LORAWAN_EV_IDLING_BEFORE_TXRX);
      return;
    }
    lorawan_handle_message(LORAWAN_EV_QUEUED_FOR_TX);
  }
}

void lorawan_task_loop(void *_)
{
  // Wait for environment sensor readings to be available.
  vTaskDelay(pdMS_TO_TICKS(ENV_SENSOR_TASK_LOOP_DELAY_MS));
  while (true)
  {
    esp_task_wdt_reset();
    // This interval must be kept extremely short, or the timing will be so off that LMIC MCCI library will be prevented from
    // receiving downlink packets.
    vTaskDelay(pdMS_TO_TICKS(3));
    lorawan_transceive();
  }
}

void lorawan_set_power_config(lorawan_power_config_t val)
{
  power_config = val;
}

lorawan_power_config_t lorawan_get_power_config()
{
  return power_config;
}