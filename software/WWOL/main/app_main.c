#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_qcloud_log.h"
#include "esp_qcloud_console.h"
#include "esp_qcloud_storage.h"
#include "esp_qcloud_iothub.h"
#include "esp_qcloud_prov.h"

#ifdef CONFIG_BT_ENABLE
#include "esp_bt.h"
#endif

#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define CONFIG_EXAMPLE_ETH_MDC_GPIO 23
#define CONFIG_EXAMPLE_ETH_MDIO_GPIO 18
#define CONFIG_EXAMPLE_ETH_PHY_RST_GPIO 5
#define CONFIG_EXAMPLE_ETH_PHY_ADDR 0

static const char *TAG = "app_main";
static esp_netif_t *l_eth_netif = NULL;
static esp_eth_handle_t l_eth_handle = NULL;
static char l_MAC_addr[512] = "B0:7B:25:16:5A:2E";

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

static int WOL(uint8_t *mac_addr) {
    struct sockaddr_in dest_addr;
    char *dst_host = "255.255.255.255";
    uint16_t dst_port = 9;
    uint8_t wol_buf[102] = {0};
    int err = 0;

    dest_addr.sin_addr.s_addr = inet_addr(dst_host);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dst_port);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    }

    for(int i=0; i<6; i++){
        wol_buf[i] = 0xFF;
    }
    int offset = 6;
    for(int i=0; i<16; i++){
        for(int j=0; j<6; j++){
            wol_buf[offset+j] = mac_addr[j];
        }
        offset += 6;
    }

    esp_netif_dhcpc_stop(l_eth_netif);
    char* ip= "127.0.0.66";
    char* gateway = "127.0.0.66";
    char* netmask = "255.255.255.0";
    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
    info_t.ip.addr = esp_ip4addr_aton((const char *)ip);
    info_t.gw.addr = esp_ip4addr_aton((const char *)gateway);
    info_t.netmask.addr = esp_ip4addr_aton((const char *)netmask);
    esp_netif_set_ip_info(l_eth_netif, &info_t);   
    esp_eth_start(l_eth_handle);

    for(int i=0; i<5; i++) {
        struct sockaddr_in saddr = { 0 };
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(0);
        saddr.sin_addr.s_addr = inet_addr("127.0.0.66");
        err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        }

        err = sendto(sock, wol_buf, sizeof(wol_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        }

        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(0);
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        }

        err = sendto(sock, wol_buf, sizeof(wol_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        }

        vTaskDelay(50 / portTICK_RATE_MS);
    }
    esp_eth_stop(l_eth_handle);

    ESP_LOGI(TAG, "Message sent");
    if (sock != -1) {
        ESP_LOGE(TAG, "Shutting down socket");
        shutdown(sock, 0);
        close(sock);
    }

    return 0;
}


/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

/* Callback to handle commands received from the QCloud cloud */
static esp_err_t wwol_get_param(const char *id, esp_qcloud_param_val_t *val)
{
    if (!strcmp(id, "MAC_addr")) {
        val->s = l_MAC_addr;
        val->type = QCLOUD_VAL_TYPE_STRING;
        ESP_LOGI(TAG, "Report id: %s, val: %s", id, val->s);
    } else if (!strcmp(id, "WOL_send")) {
        val->s = l_MAC_addr;
        val->type = QCLOUD_VAL_TYPE_STRING;
        ESP_LOGI(TAG, "Report id: %s, val: %s", id, val->s);
    } else {
        ESP_LOGI(TAG, "Report id: %s, val: %d", id, val->i);
    }

    return ESP_OK;
}

/* Callback to handle commands received from the QCloud cloud */
static esp_err_t wwol_set_param(const char *id, const esp_qcloud_param_val_t *val)
{
    esp_err_t err = ESP_FAIL;
    

    if (!strcmp(id, "MAC_addr")) {
        uint8_t mac[6] = {0};
        sscanf(l_MAC_addr, "%02X:%02X:%02X:%02X:%02X:%02X", 
            &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        WOL(mac);
        err = ESP_OK;
        ESP_LOGI(TAG, "Received id: %s, val: %s", id, val->s);
    } else if (!strcmp(id, "WOL_send")) {
        strncpy(l_MAC_addr, val->s, sizeof(l_MAC_addr));
        WOL(mac);
        err = ESP_OK;
        ESP_LOGI(TAG, "Received id: %s, val: %s", id, val->s);
    }else {
        ESP_LOGI(TAG, "Received id: %s, val: %d", id, val->i);
    }

    return err;
}

/* Event handler for catching QCloud events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    switch (event_id) {
        case QCLOUD_EVENT_IOTHUB_INIT_DONE:
            esp_qcloud_iothub_report_device_info();
            ESP_LOGI(TAG, "QCloud Initialised");
            break;

        case QCLOUD_EVENT_IOTHUB_BOND_DEVICE:
            ESP_LOGI(TAG, "Device binding successful");
            break;

        case QCLOUD_EVENT_IOTHUB_UNBOND_DEVICE:
            ESP_LOGW(TAG, "Device unbound with iothub");
            esp_qcloud_wifi_reset();
            esp_restart();
            break;

        case QCLOUD_EVENT_IOTHUB_BIND_EXCEPTION:
            ESP_LOGW(TAG, "Device bind fail");
            esp_qcloud_wifi_reset();
            esp_restart();
            break;
            
        case QCLOUD_EVENT_IOTHUB_RECEIVE_STATUS:
            ESP_LOGI(TAG, "receive status message: %s",(char*)event_data);
            break;

        default:
            ESP_LOGW(TAG, "Unhandled QCloud Event: %d", event_id);
    }
}

static esp_err_t get_wifi_config(wifi_config_t *wifi_cfg, uint32_t wait_ms)
{
    ESP_QCLOUD_PARAM_CHECK(wifi_cfg);

    if (esp_qcloud_storage_get("wifi_config", wifi_cfg, sizeof(wifi_config_t)) == ESP_OK) {

#ifdef CONFIG_BT_ENABLE
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif

        return ESP_OK;
    }

    /**< Reset wifi and restart wifi */
    esp_wifi_restore();
    esp_wifi_start();

    /**< Note: Smartconfig and softapconfig working at the same time will affect the configure network performance */

#ifdef CONFIG_LIGHT_PROVISIONING_SOFTAPCONFIG
    char softap_ssid[32 + 1] = CONFIG_LIGHT_PROVISIONING_SOFTAPCONFIG_SSID;
    // uint8_t mac[6] = {0};
    // ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    // sprintf(softap_ssid, "tcloud_%s_%02x%02x", light_driver_get_type(), mac[4], mac[5]);

    esp_qcloud_prov_softapconfig_start(SOFTAPCONFIG_TYPE_ESPRESSIF_TENCENT,
                                       softap_ssid,
                                       CONFIG_LIGHT_PROVISIONING_SOFTAPCONFIG_PASSWORD);
    esp_qcloud_prov_print_wechat_qr(softap_ssid, "softap");
#endif

#ifdef CONFIG_LIGHT_PROVISIONING_SMARTCONFIG
    esp_qcloud_prov_smartconfig_start(SC_TYPE_ESPTOUCH_AIRKISS);
#endif

#ifdef CONFIG_LIGHT_PROVISIONING_BLECONFIG
    char local_name[32 + 1] = CONFIG_LIGHT_PROVISIONING_BLECONFIG_NAME;
    esp_qcloud_prov_bleconfig_start(BLECONFIG_TYPE_ESPRESSIF_TENCENT, local_name);
#endif

    ESP_ERROR_CHECK(esp_qcloud_prov_wait(wifi_cfg, wait_ms));

#ifdef CONFIG_LIGHT_PROVISIONING_SMARTCONFIG
    esp_qcloud_prov_smartconfig_stop();
#endif

#ifdef CONFIG_LIGHT_PROVISIONING_SOFTAPCONFIG
    esp_qcloud_prov_softapconfig_stop();
#endif

    /**< Store the configure of the device */
    esp_qcloud_storage_set("wifi_config", wifi_cfg, sizeof(wifi_config_t));


    return ESP_OK;
}

void app_main()
{
    /**
     * @brief Add debug function, you can use serial command and remote debugging.
     */
    esp_qcloud_log_config_t log_config = {
        .log_level_uart = ESP_LOG_INFO,
    };
    ESP_ERROR_CHECK(esp_qcloud_log_init(&log_config));
    /**
     * @brief Set log level
     * @note  This function can not raise log level above the level set using
     * CONFIG_LOG_DEFAULT_LEVEL setting in menuconfig.
     */
    esp_log_level_set("*", ESP_LOG_VERBOSE);

#ifdef CONFIG_LIGHT_DEBUG
    ESP_ERROR_CHECK(esp_qcloud_console_init());
    esp_qcloud_print_system_info(10000);
#endif /**< CONFIG_LIGHT_DEBUG */

    /**< Continuous power off and restart more than five times to reset the device */
    if (esp_qcloud_reboot_unbroken_count() >= CONFIG_LIGHT_REBOOT_UNBROKEN_COUNT_RESET) {
        ESP_LOGW(TAG, "Erase information saved in flash");
        esp_qcloud_storage_erase(CONFIG_QCLOUD_NVS_NAMESPACE);
    } else if (esp_qcloud_reboot_is_exception(false)) {
        ESP_LOGE(TAG, "The device has been restarted abnormally");
    }

    /*
     * @breif Create a device through the server and obtain configuration parameters
     *        server: https://console.cloud.tencent.com/iotexplorer
     */
    /**< Create and configure device authentication information */
    ESP_ERROR_CHECK(esp_qcloud_create_device());
    /**< Configure the version of the device, and use this information to determine whether to OTA */
    ESP_ERROR_CHECK(esp_qcloud_device_add_fw_version("0.0.1"));
    /**< Register the properties of the device */
    ESP_ERROR_CHECK(esp_qcloud_device_add_property("MAC_addr", QCLOUD_VAL_TYPE_STRING));
    /**< The processing function of the communication between the device and the server */
    ESP_ERROR_CHECK(esp_qcloud_device_add_property_cb(wwol_get_param, wwol_set_param));
    
    /**
     * @brief Initialize Wi-Fi.
     */
    ESP_ERROR_CHECK(esp_qcloud_wifi_init());
    ESP_ERROR_CHECK(esp_event_handler_register(QCLOUD_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    l_eth_netif = esp_netif_new(&cfg);

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(l_eth_netif));

    char* ip= "127.0.0.66";
    char* gateway = "127.0.0.66";
    char* netmask = "255.255.255.0";
    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
    info_t.ip.addr = esp_ip4addr_aton((const char *)ip);
    info_t.gw.addr = esp_ip4addr_aton((const char *)gateway);
    info_t.netmask.addr = esp_ip4addr_aton((const char *)netmask);
    esp_netif_set_ip_info(l_eth_netif, &info_t);    

    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = CONFIG_EXAMPLE_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_EXAMPLE_ETH_PHY_RST_GPIO;
    mac_config.smi_mdc_gpio_num = CONFIG_EXAMPLE_ETH_MDC_GPIO;
    mac_config.smi_mdio_gpio_num = CONFIG_EXAMPLE_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &l_eth_handle));
    /* attach Ethernet driver to TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_attach(l_eth_netif, esp_eth_new_netif_glue(l_eth_handle)));

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    /**
     * @brief Get the router configuration
     */
    wifi_config_t wifi_cfg = {0};
    ESP_ERROR_CHECK(get_wifi_config(&wifi_cfg, portMAX_DELAY));

    /**
     * @brief Connect to router
     */
    ESP_ERROR_CHECK(esp_qcloud_wifi_start(&wifi_cfg));
    ESP_ERROR_CHECK(esp_qcloud_timesync_start());

    /**
     * @brief Connect to Tencent Cloud Iothub
     */
    ESP_ERROR_CHECK(esp_qcloud_iothub_init());
    ESP_ERROR_CHECK(esp_qcloud_iothub_start());
    ESP_ERROR_CHECK(esp_qcloud_iothub_ota_enable());

}
