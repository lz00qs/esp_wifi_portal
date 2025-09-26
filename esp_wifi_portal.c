#include <stdio.h>
#include "esp_wifi_portal.h"

#include <esp_http_server.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <lwip/inet.h>

#include "dns_server.h"
#include "http_server.h"

static const char* TAG = "esp_wifi_portal";

static esp_netif_t* sta_netif = NULL;

static esp_netif_t* ap_netif = NULL;

static bool is_auto_start = true;

static bool is_portal_running = false;

EventGroupHandle_t wifi_event_group = NULL;

static dns_server_handle_t dns_server = NULL;

static esp_event_handler_instance_t sta_event_handler_wifi_instance = NULL;
static esp_event_handler_instance_t sta_event_handler_ip_instance = NULL;

/**
 * @brief Event handler for station mode WiFi events
 *
 * @param arg Argument passed to the event handler (unused)
 * @param event_base Base of the event (WIFI_EVENT or IP_EVENT)
 * @param event_id ID of the event (specific to the event base)
 * @param event_data Data associated with the event (event-specific)
 */
static void sta_event_handler(void* arg, const esp_event_base_t event_base,
                              const int32_t event_id, void* event_data)
{
    if (is_portal_running == false)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "Wifi STA Started");
            esp_wifi_connect();
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            ESP_LOGI(TAG, "Wifi STA Disconnected");
            if (is_auto_start)
            {
                esp_wifi_portal_start();
            }
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            const ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        }
    }
}

static esp_event_handler_instance_t ap_event_handler_wifi_instance;
static esp_event_handler_instance_t ap_event_handler_ip_instance;

/**
 * @brief Event handler for access point mode WiFi events
 *
 * @param arg Argument passed to the event handler (unused)
 * @param event_base Base of the event (WIFI_EVENT or IP_EVENT)
 * @param event_id ID of the event (specific to the event base)
 * @param event_data Data associated with the event (event-specific)
 */
static void ap_event_handler(void* arg, const esp_event_base_t event_base,
                             const int32_t event_id, void* event_data)
{
    if (is_portal_running == true)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
        {
            ESP_LOGI(TAG, "Wifi AP Started");
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            const ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            if (wifi_event_group != NULL) { xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT); }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_wifi_portal_stop();
        }
    }
}

/**
 * @brief Register event handlers for station mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t registerStaEventHandlers(void)
{
    ESP_LOGI(TAG, "register sta event handlers");
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &sta_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register sta WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &sta_event_handler,
                                              NULL,
                                              &sta_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register sta IP_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Unregister event handlers for station mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t unregisterStaEventHandlers(void)
{
    ESP_LOGI(TAG, "unregister sta event handlers");
    esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          sta_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister sta WIFI_EVENT event WIFI_EVENT_STA_START handler failed, err: %d", err);
        return err;
    }
    err = esp_event_handler_instance_unregister(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                sta_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister sta IP_EVENT event handler failed, err: %d", err);
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Create the station network interface
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static void create_sta_netif(void)
{
    if (sta_netif == NULL)
    {
        // First time create
        sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);
        wifi_config_t sta_cfg;
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg); // 读出当前配置
        sta_cfg.sta.failure_retry_cnt = CONFIG_ESP_WIFI_PORTAL_STA_RETRY_CNT; // 修改重试次数
        // 当 ssid 为空的时候提供一个默认的 ssid 触发连接
        if (strlen((const char*)sta_cfg.sta.ssid) == 0)
        {
            ESP_LOGW(TAG, "sta ssid is empty, use default ssid: %s", "ap");
            strcpy((char*)sta_cfg.sta.ssid, "ap");
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg)); // 写回配置
    }
}

/**
 * @brief Register event handlers for access point mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t registerApEventHandlers(void)
{
    esp_err_t err = esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ap_event_handler,
                                                        NULL,
                                                        &ap_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register ap IP_EVENT event handler failed, err: %d", err);
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              WIFI_EVENT_AP_START,
                                              &ap_event_handler,
                                              NULL,
                                              &ap_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register ap WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Unregister event handlers for access point mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t unregisterApEventHandlers(void)
{
    esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          ap_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister ap IP_EVENT event handler failed, err: %d", err);
        return err;
    }


    err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                WIFI_EVENT_AP_START,
                                                ap_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister ap WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Create the access point network interface
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static void create_ap_netif(void)
{
    if (ap_netif == NULL)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        assert(ap_netif);

        wifi_config_t wifi_ap_cfg = {
            .ap = {
                .ssid = CONFIG_ESP_WIFI_PORTAL_AP_SSID,
                .password = CONFIG_ESP_WIFI_PORTAL_AP_PASSWORD,
                .max_connection = 1,
                .authmode = WIFI_AUTH_WPA3_PSK
            },
        };

        if (strlen(CONFIG_ESP_WIFI_PORTAL_AP_PASSWORD) == 0)
        {
            wifi_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_cfg));
        ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
        esp_netif_ip_info_t ip_info = {0};
#if (CONFIG_ESP_WIFI_PORTAL_AP_ENHANCED_CAPTIVE)
        ip_info.ip.addr = esp_ip4addr_aton("8.8.8.8");
        ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        ip_info.gw.addr = esp_ip4addr_aton("8.8.8.8");
#else
        ip_info.ip.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_IP);
        ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_NETMASK);
        ip_info.gw.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_GATEWAY);
#endif

        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
        ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

        ESP_LOGI(TAG, "AP SSID: %s, password: %s", wifi_ap_cfg.ap.ssid, wifi_ap_cfg.ap.password);
    }
}

/**
 * @brief Initialize the Wi-Fi portal
 * @note Call this function after nvs_flash_init(), esp_netif_init() and esp_event_loop_create_default()
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_wifi_portal_init(void)
{
    ESP_LOGI(TAG, "esp_wifi_portal_init");
    /*Initialize WiFi */
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(registerStaEventHandlers());
    ESP_ERROR_CHECK(registerApEventHandlers());
    create_sta_netif();
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

#ifdef CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL
static void dhcp_set_captive_portal_url(void)
{
    // get the IP of the access point to redirect to
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    // turn the IP into a URI
    char* captive_portal_uri = (char*)malloc(32 * sizeof(char));
    assert(captive_portal_uri && "Failed to allocate captive_portal_uri");
    strcpy(captive_portal_uri, "http://");
    strcat(captive_portal_uri, ip_addr);

    // get a handle to configure DHCP with
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    // set the DHCP option 114
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captive_portal_uri, strlen(
            captive_portal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}
#endif // CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL

esp_err_t esp_wifi_portal_start(void)
{
    if (is_portal_running == true)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_start: portal already running");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    create_ap_netif();

#ifdef CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL
    dhcp_set_captive_portal_url();
#endif

    if (wifi_event_group == NULL)
    {
        wifi_event_group = xEventGroupCreate();
    }

    const esp_err_t ret = start_webserver(wifi_event_group);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    dns_server = start_dns_server(&config);
    if (dns_server == NULL)
    {
        ESP_LOGE(TAG, "Failed to start DNS server");
        return ESP_FAIL;
    }
    is_portal_running = true;
    return ESP_OK;
}

esp_err_t esp_wifi_portal_stop(void)
{
    if (is_portal_running == false)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_stop: portal not running");
        return ESP_FAIL;
    }
    is_portal_running = false;

    stop_dns_server(dns_server);
    dns_server = NULL;
    ESP_ERROR_CHECK(stop_webserver());
    vEventGroupDelete(wifi_event_group);
    wifi_event_group = NULL;
    // 如果 Log level 是 info 打印 wifi config
#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_INFO
    wifi_config_t wifi_sta_cfg;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_sta_cfg));
    ESP_LOGI(TAG, "Station SSID: %s, password: %s", wifi_sta_cfg.sta.ssid, wifi_sta_cfg.sta.password);
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_destroy(ap_netif);
    ap_netif = NULL;
    return ESP_OK;
}

esp_err_t esp_wifi_portal_deinit(void)
{
    ESP_LOGI(TAG, "esp_wifi_portal_deinit");
    if (is_portal_running == true)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_deinit: portal running, stopping it");
        esp_wifi_portal_stop();
    }
    ESP_ERROR_CHECK(unregisterStaEventHandlers());
    ESP_ERROR_CHECK(unregisterApEventHandlers());
    ESP_ERROR_CHECK(esp_wifi_stop());
    if (wifi_event_group != NULL)
    {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
    if (ap_netif != NULL)
    {
        esp_netif_destroy(ap_netif);
        ap_netif = NULL;
    }
    if (sta_netif != NULL)
    {
        esp_netif_destroy(sta_netif);
        sta_netif = NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_deinit());

    return ESP_OK;
}

/**
 * @brief Set whether the portal should start automatically when the station disconnects
 * @param auto_start true to start automatically, false to not start automatically
 */
void esp_wifi_portal_set_auto_start(const bool auto_start)
{
    is_auto_start = auto_start;
}
