/**
 * @file main.c
 * @brief PM sensor dashboard over MicroLink/Tailscale.
 *
 * The device joins Wi-Fi, connects to a Tailscale tailnet through MicroLink,
 * samples an SPS30 particulate sensor, and serves a browser-rendered dashboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "microlink.h"
#include "microlink_internal.h"
#include "ml_config_httpd.h"
#include "pm_credentials.h"
#include "pm_sps30.h"

static const char *TAG = "main";

#define HTTP_PORT 80
#define MSG_PORT 9000
#define MSG_SEND_INTERVAL_MS 5000
#define SENSOR_INTERVAL_MS 2000
#define SENSOR_HISTORY_LEN 1800
#define HOURLY_RECORDS_LEN 168
#define HOURLY_WINDOW_MS (60LL * 60LL * 1000LL)
#define BOARD_POWER_ON_GPIO 15

#define PM_HOURLY_NVS_NAMESPACE "pm_hourly"
#define PM_HOURLY_NVS_KEY "records"
#define PM_HOURLY_MAGIC 0x504D4831u
#define PM_HOURLY_VERSION 1u

static char wifi_ssid[33] = PM_WIFI_SSID;
static char wifi_password[65] = PM_WIFI_PASSWORD;

static ml_config_wifi_list_t wifi_list;
static int wifi_list_count;
static int current_wifi_idx;
static int wifi_retry_count;
#define WIFI_MAX_RETRIES_PER_SSID 3

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static char wifi_ip_str[16] = "0.0.0.0";

static microlink_t *ml;
static microlink_udp_socket_t *udp_sock;
static httpd_handle_t dashboard_httpd;
static TaskHandle_t peer_warm_task_handle;

static uint32_t msg_tx_count;
static uint32_t msg_rx_count;

static SemaphoreHandle_t sensor_mutex;
static SemaphoreHandle_t hourly_mutex;
static pm_sps30_sample_t latest_sample;
static pm_sps30_sample_t *history;
static uint16_t history_head;
static uint16_t history_count;
static bool latest_valid;
static uint32_t sensor_read_failures;

typedef struct {
    uint32_t seq;
    int64_t start_ms;
    int64_t end_ms;
    uint32_t sample_count;
    float pm1_0;
    float pm2_5;
    float pm4_0;
    float pm10_0;
} pm_hourly_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t head;
    uint16_t reserved;
    uint32_t next_seq;
    pm_hourly_record_t records[HOURLY_RECORDS_LEN];
} pm_hourly_store_t;

typedef struct {
    int64_t start_ms;
    uint32_t count;
    double pm1_0;
    double pm2_5;
    double pm4_0;
    double pm10_0;
} pm_hourly_acc_t;

static pm_hourly_store_t hourly_store;
static pm_hourly_acc_t hourly_acc;
static nvs_handle_t hourly_nvs;
static bool hourly_nvs_ready;

static const char DASHBOARD_HTML[] =
"<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>PM MicroLink</title><style>"
":root{color-scheme:dark;--bg:#0e1116;--panel:#171c24;--line:#2a313d;--text:#edf1f7;--muted:#9aa6b2;--good:#39d98a;--warn:#ffce56;--bad:#ff6b6b;--accent:#5eb1ff}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;letter-spacing:0}"
"header{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:18px 22px;border-bottom:1px solid var(--line);background:#111720}"
"h1{font-size:20px;margin:0;font-weight:650}.sub{color:var(--muted);font-size:13px}.wrap{max-width:1180px;margin:0 auto;padding:20px;display:grid;gap:16px}"
".top{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;min-width:0}"
".metric .label{color:var(--muted);font-size:12px}.metric .value{font-size:28px;font-weight:700;margin-top:6px}.metric .unit{font-size:13px;color:var(--muted);margin-left:4px}"
".grid{display:grid;grid-template-columns:2fr 1fr;gap:16px}.chart{height:330px}.status{display:grid;gap:10px}.row{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid #232a34;padding:8px 0}.row:last-child{border-bottom:0}.row span:first-child{color:var(--muted)}"
"canvas{width:100%;height:100%;display:block}.pill{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--line);border-radius:999px;padding:6px 10px;color:var(--muted);white-space:nowrap}.dot{width:8px;height:8px;border-radius:50%;background:var(--warn)}.dot.ok{background:var(--good)}.dot.bad{background:var(--bad)}"
".table{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:8px}.small .value{font-size:20px}.foot{color:var(--muted);font-size:12px;padding-bottom:10px}"
"@media(max-width:860px){.top{grid-template-columns:repeat(2,minmax(0,1fr))}.grid{grid-template-columns:1fr}.table{grid-template-columns:repeat(2,minmax(0,1fr))}header{align-items:flex-start;flex-direction:column}.chart{height:280px}}"
"</style></head><body><header><div><h1>PM MicroLink</h1><div class=\"sub\">SPS30 particulate sensor over Tailscale</div></div><div class=\"pill\"><i id=\"dot\" class=\"dot\"></i><span id=\"state\">starting</span></div></header>"
"<main class=\"wrap\"><section class=\"top\">"
"<div class=\"card metric\"><div class=\"label\">PM2.5</div><div class=\"value\"><span id=\"pm25\">--</span><span class=\"unit\">ug/m3</span></div></div>"
"<div class=\"card metric\"><div class=\"label\">PM10</div><div class=\"value\"><span id=\"pm10\">--</span><span class=\"unit\">ug/m3</span></div></div>"
"<div class=\"card metric\"><div class=\"label\">PM1.0</div><div class=\"value\"><span id=\"pm1\">--</span><span class=\"unit\">ug/m3</span></div></div>"
"<div class=\"card metric\"><div class=\"label\">Particle Size</div><div class=\"value\"><span id=\"size\">--</span><span class=\"unit\">um</span></div></div>"
"</section><section class=\"grid\"><div class=\"card chart\"><canvas id=\"chart\"></canvas></div><div class=\"card status\">"
"<div class=\"row\"><span>Tailnet IP</span><b id=\"vpn\">--</b></div><div class=\"row\"><span>Wi-Fi RSSI</span><b id=\"rssi\">--</b></div><div class=\"row\"><span>Peers</span><b id=\"peers\">--</b></div><div class=\"row\"><span>Uptime</span><b id=\"uptime\">--</b></div><div class=\"row\"><span>Reads</span><b id=\"reads\">--</b></div><div class=\"row\"><span>Hourly</span><b id=\"hourly\">--</b></div><div class=\"row\"><span>Sensor</span><b id=\"sensor\">--</b></div>"
"</div></section><section class=\"table\">"
"<div class=\"card metric small\"><div class=\"label\">PM4.0</div><div class=\"value\"><span id=\"pm4\">--</span><span class=\"unit\">ug/m3</span></div></div>"
"<div class=\"card metric small\"><div class=\"label\">NC0.5</div><div class=\"value\"><span id=\"nc05\">--</span><span class=\"unit\">#/cm3</span></div></div>"
"<div class=\"card metric small\"><div class=\"label\">NC1.0</div><div class=\"value\"><span id=\"nc10\">--</span><span class=\"unit\">#/cm3</span></div></div>"
"<div class=\"card metric small\"><div class=\"label\">NC2.5</div><div class=\"value\"><span id=\"nc25\">--</span><span class=\"unit\">#/cm3</span></div></div>"
"<div class=\"card metric small\"><div class=\"label\">NC10</div><div class=\"value\"><span id=\"nc100\">--</span><span class=\"unit\">#/cm3</span></div></div>"
"</section><div class=\"foot\">HTTP port 80. Data refreshes in the browser; the device serves only static HTML and JSON.</div></main>"
"<script>"
"const $=id=>document.getElementById(id),fmt=(v,d=1)=>Number.isFinite(v)?v.toFixed(d):'--';let hist=[];"
"function up(m){const ok=m.sensor&&m.sensor.state==='measuring'&&m.latest; $('dot').className='dot '+(ok?'ok':(m.sensor&&m.sensor.state==='error'?'bad':'')); $('state').textContent=ok?'live':(m.sensor?m.sensor.state:'offline');"
"if(m.latest){let x=m.latest;$('pm25').textContent=fmt(x.pm2_5);$('pm10').textContent=fmt(x.pm10_0);$('pm1').textContent=fmt(x.pm1_0);$('pm4').textContent=fmt(x.pm4_0);$('size').textContent=fmt(x.typical_particle_size,2);$('nc05').textContent=fmt(x.nc0_5,0);$('nc10').textContent=fmt(x.nc1_0,0);$('nc25').textContent=fmt(x.nc2_5,0);$('nc100').textContent=fmt(x.nc10_0,0);}"
"$('vpn').textContent=m.vpn_ip||'--';$('rssi').textContent=m.wifi_rssi?m.wifi_rssi+' dBm':'--';$('peers').textContent=m.peer_count;let s=Math.floor(m.uptime_ms/1000);$('uptime').textContent=Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';$('reads').textContent=(m.sensor.read_count||0)+' / '+(m.sensor.error_count||0);$('hourly').textContent=(m.hourly?m.hourly.stored:0)+' saved';$('sensor').textContent=m.sensor.state;}"
"function pushLatest(x){if(!x||!Number.isFinite(x.timestamp_ms))return;let last=hist[hist.length-1];if(last&&last.t===x.timestamp_ms)return;hist.push({t:x.timestamp_ms,pm1:x.pm1_0,pm25:x.pm2_5,pm10:x.pm10_0});if(hist.length>1800)hist.shift();draw()}"
"async function poll(){try{let m=await fetch('/api/metrics',{cache:'no-store'}).then(r=>r.json());up(m);pushLatest(m.latest)}catch(e){$('state').textContent='offline';$('dot').className='dot bad'}}"
"async function loadHist(){try{hist=(await fetch('/api/history',{cache:'no-store'}).then(r=>r.json())).samples||[];draw()}catch(e){}}"
"function draw(){const c=$('chart'),r=c.getBoundingClientRect(),d=window.devicePixelRatio||1;c.width=Math.max(1,r.width*d);c.height=Math.max(1,r.height*d);const g=c.getContext('2d');g.scale(d,d);g.clearRect(0,0,r.width,r.height);g.strokeStyle='#2a313d';g.lineWidth=1;for(let i=0;i<5;i++){let y=24+i*(r.height-48)/4;g.beginPath();g.moveTo(42,y);g.lineTo(r.width-12,y);g.stroke()}let max=Math.max(10,...hist.flatMap(p=>[p.pm1,p.pm25,p.pm10]).filter(Number.isFinite));const lines=[['pm25','#5eb1ff'],['pm10','#ffce56'],['pm1','#39d98a']];"
"g.font='12px system-ui';g.fillStyle='#9aa6b2';g.fillText('PM trend ug/m3',42,16);lines.forEach(([k,col],li)=>{g.strokeStyle=col;g.lineWidth=2;g.beginPath();hist.forEach((p,i)=>{let x=42+i*Math.max(1,(r.width-58))/Math.max(1,hist.length-1),y=r.height-24-(p[k]/max)*(r.height-58);i?g.lineTo(x,y):g.moveTo(x,y)});g.stroke();g.fillStyle=col;g.fillText(k,42+li*58,r.height-8)})}"
"setInterval(poll,2000);setInterval(loadHist,10000);addEventListener('resize',draw);poll();loadHist();"
"</script></body></html>";

static const char *ml_state_name(microlink_state_t state) {
    switch (state) {
        case ML_STATE_IDLE: return "idle";
        case ML_STATE_WIFI_WAIT: return "wifi_wait";
        case ML_STATE_CONNECTING: return "connecting";
        case ML_STATE_REGISTERING: return "registering";
        case ML_STATE_CONNECTED: return "connected";
        case ML_STATE_RECONNECTING: return "reconnecting";
        case ML_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static void board_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_POWER_ON_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_POWER_ON_GPIO, 1);
}

static void history_add(const pm_sps30_sample_t *sample) {
    if (!sample || !sensor_mutex || !history) return;

    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latest_sample = *sample;
        latest_valid = true;
        history[history_head] = *sample;
        history_head = (history_head + 1) % SENSOR_HISTORY_LEN;
        if (history_count < SENSOR_HISTORY_LEN) {
            history_count++;
        }
        xSemaphoreGive(sensor_mutex);
    }
}

static void hourly_store_reset(void) {
    memset(&hourly_store, 0, sizeof(hourly_store));
    hourly_store.magic = PM_HOURLY_MAGIC;
    hourly_store.version = PM_HOURLY_VERSION;
    hourly_store.next_seq = 1;
}

static esp_err_t hourly_store_save_locked(void) {
    if (!hourly_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_blob(hourly_nvs, PM_HOURLY_NVS_KEY,
                                 &hourly_store, sizeof(hourly_store));
    if (err == ESP_OK) {
        err = nvs_commit(hourly_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hourly NVS save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void hourly_store_init(void) {
    hourly_mutex = xSemaphoreCreateMutex();
    hourly_store_reset();

    esp_err_t err = nvs_open(PM_HOURLY_NVS_NAMESPACE, NVS_READWRITE, &hourly_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hourly NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    hourly_nvs_ready = true;

    size_t len = sizeof(hourly_store);
    err = nvs_get_blob(hourly_nvs, PM_HOURLY_NVS_KEY, &hourly_store, &len);
    if (err != ESP_OK || len != sizeof(hourly_store) ||
        hourly_store.magic != PM_HOURLY_MAGIC ||
        hourly_store.version != PM_HOURLY_VERSION ||
        hourly_store.count > HOURLY_RECORDS_LEN ||
        hourly_store.head >= HOURLY_RECORDS_LEN) {
        hourly_store_reset();
        hourly_store_save_locked();
        ESP_LOGI(TAG, "Hourly PM store initialized (%d records)", HOURLY_RECORDS_LEN);
    } else {
        ESP_LOGI(TAG, "Hourly PM store loaded: %u records", hourly_store.count);
    }
}

static void hourly_finalize_locked(int64_t end_ms) {
    if (hourly_acc.count == 0) {
        return;
    }

    pm_hourly_record_t rec = {
        .seq = hourly_store.next_seq++,
        .start_ms = hourly_acc.start_ms,
        .end_ms = end_ms,
        .sample_count = hourly_acc.count,
        .pm1_0 = (float)(hourly_acc.pm1_0 / hourly_acc.count),
        .pm2_5 = (float)(hourly_acc.pm2_5 / hourly_acc.count),
        .pm4_0 = (float)(hourly_acc.pm4_0 / hourly_acc.count),
        .pm10_0 = (float)(hourly_acc.pm10_0 / hourly_acc.count),
    };

    hourly_store.records[hourly_store.head] = rec;
    hourly_store.head = (hourly_store.head + 1) % HOURLY_RECORDS_LEN;
    if (hourly_store.count < HOURLY_RECORDS_LEN) {
        hourly_store.count++;
    }

    ESP_LOGI(TAG, "Hourly PM saved: seq=%lu samples=%lu PM2.5=%.2f PM10=%.2f",
             (unsigned long)rec.seq,
             (unsigned long)rec.sample_count,
             rec.pm2_5,
             rec.pm10_0);

    hourly_store_save_locked();
    memset(&hourly_acc, 0, sizeof(hourly_acc));
}

static void hourly_add_sample(const pm_sps30_sample_t *sample) {
    if (!sample || !hourly_mutex) {
        return;
    }

    if (xSemaphoreTake(hourly_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (hourly_acc.count > 0 &&
        sample->timestamp_ms - hourly_acc.start_ms >= HOURLY_WINDOW_MS) {
        hourly_finalize_locked(sample->timestamp_ms);
    }

    if (hourly_acc.count == 0) {
        hourly_acc.start_ms = sample->timestamp_ms;
    }

    hourly_acc.count++;
    hourly_acc.pm1_0 += sample->pm1_0;
    hourly_acc.pm2_5 += sample->pm2_5;
    hourly_acc.pm4_0 += sample->pm4_0;
    hourly_acc.pm10_0 += sample->pm10_0;

    xSemaphoreGive(hourly_mutex);
}

static void sensor_task(void *arg) {
    (void)arg;
    int consecutive_failures = 0;

    while (1) {
        if (pm_sps30_init() == ESP_OK) {
            consecutive_failures = 0;
            break;
        }
        consecutive_failures++;
        ESP_LOGW(TAG, "SPS30 init retry in 5s (failures=%d)", consecutive_failures);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    while (1) {
        pm_sps30_sample_t sample;
        esp_err_t err = pm_sps30_read(&sample);
        if (err == ESP_OK) {
            consecutive_failures = 0;
            history_add(&sample);
            hourly_add_sample(&sample);
        } else {
            sensor_read_failures++;
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                ESP_LOGW(TAG, "too many SPS30 read failures, reinitializing");
                pm_sps30_stop();
                while (pm_sps30_init() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
                consecutive_failures = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

static esp_err_t handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_metrics(httpd_req_t *req) {
    char vpn_ip[16] = "";
    uint32_t ip = ml ? microlink_get_vpn_ip(ml) : 0;
    if (ip) {
        microlink_ip_to_str(ip, vpn_ip);
    }

    wifi_ap_record_t ap = {0};
    int wifi_rssi = 0;
    bool has_rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (has_rssi) {
        wifi_rssi = ap.rssi;
    }
    char rssi_json[16];
    if (has_rssi) {
        snprintf(rssi_json, sizeof(rssi_json), "%d", wifi_rssi);
    } else {
        snprintf(rssi_json, sizeof(rssi_json), "null");
    }

    pm_sps30_sample_t sample = {0};
    bool has_sample = false;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sample = latest_sample;
        has_sample = latest_valid;
        xSemaphoreGive(sensor_mutex);
    }

    pm_sps30_status_t st;
    pm_sps30_get_status(&st);

    uint16_t hourly_stored = 0;
    uint32_t hourly_active_samples = 0;
    float hourly_active_pm25 = 0.0f;
    if (hourly_mutex && xSemaphoreTake(hourly_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hourly_stored = hourly_store.count;
        hourly_active_samples = hourly_acc.count;
        if (hourly_acc.count > 0) {
            hourly_active_pm25 = (float)(hourly_acc.pm2_5 / hourly_acc.count);
        }
        xSemaphoreGive(hourly_mutex);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    int n = snprintf(buf, 2048,
        "{\"device\":\"%s\",\"vpn_ip\":\"%s\",\"ml_state\":\"%s\","
        "\"peer_count\":%d,\"uptime_ms\":%lld,\"heap_free\":%lu,"
        "\"psram_free\":%lu,\"wifi_rssi\":%s,"
        "\"sensor\":{\"state\":\"%s\",\"last_error\":%d,\"error_count\":%lu,"
        "\"read_count\":%lu,\"app_read_failures\":%lu},"
        "\"hourly\":{\"stored\":%u,\"active_samples\":%lu,\"active_pm25\":%.2f},"
        "\"latest\":",
        PM_DEVICE_NAME[0] ? PM_DEVICE_NAME : "pm-microlink",
        vpn_ip,
        ml ? ml_state_name(microlink_get_state(ml)) : "offline",
        ml ? microlink_get_peer_count(ml) : 0,
        (long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        rssi_json,
        pm_sps30_state_name(st.state),
        st.last_error,
        (unsigned long)st.error_count,
        (unsigned long)st.read_count,
        (unsigned long)sensor_read_failures,
        hourly_stored,
        (unsigned long)hourly_active_samples,
        hourly_active_pm25);

    if (n < 0 || n >= 2048) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, buf, n);

    if (has_sample) {
        n = snprintf(buf, 2048,
            "{\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10_0\":%.2f,"
            "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,"
            "\"nc10_0\":%.2f,\"typical_particle_size\":%.3f,\"timestamp_ms\":%lld}}",
            sample.pm1_0, sample.pm2_5, sample.pm4_0, sample.pm10_0,
            sample.nc0_5, sample.nc1_0, sample.nc2_5, sample.nc4_0,
            sample.nc10_0, sample.typical_particle_size,
            (long long)sample.timestamp_ms);
    } else {
        n = snprintf(buf, 2048, "null}");
    }
    if (n < 0 || n >= 2048) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, buf, n);
    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return err;
}

static esp_err_t handler_history(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"interval_ms\":2000,\"samples\":[");

    uint16_t count = 0;
    uint16_t head = 0;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = history_count;
        head = history_head;
        xSemaphoreGive(sensor_mutex);
    }

    for (uint16_t i = 0; i < count; i++) {
        pm_sps30_sample_t s = {0};
        uint16_t idx = (head + SENSOR_HISTORY_LEN - count + i) % SENSOR_HISTORY_LEN;
        if (history && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s = history[idx];
            xSemaphoreGive(sensor_mutex);
        }

        char item[180];
        int n = snprintf(item, sizeof(item),
                         "%s{\"t\":%lld,\"pm1\":%.2f,\"pm25\":%.2f,\"pm10\":%.2f}",
                         i ? "," : "",
                         (long long)s.timestamp_ms,
                         s.pm1_0,
                         s.pm2_5,
                         s.pm10_0);
        if (n > 0) {
            httpd_resp_send_chunk(req, item, n);
        }
    }

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handler_hourly(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!hourly_mutex || xSemaphoreTake(hourly_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "hourly store busy");
        return ESP_FAIL;
    }

    char head[256];
    int n = snprintf(head, sizeof(head),
                     "{\"window_ms\":%lld,\"stored\":%u,\"capacity\":%u,"
                     "\"active\":{\"start_ms\":%lld,\"sample_count\":%lu,"
                     "\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10_0\":%.2f},"
                     "\"records\":[",
                     (long long)HOURLY_WINDOW_MS,
                     hourly_store.count,
                     HOURLY_RECORDS_LEN,
                     (long long)hourly_acc.start_ms,
                     (unsigned long)hourly_acc.count,
                     hourly_acc.count ? (float)(hourly_acc.pm1_0 / hourly_acc.count) : 0.0f,
                     hourly_acc.count ? (float)(hourly_acc.pm2_5 / hourly_acc.count) : 0.0f,
                     hourly_acc.count ? (float)(hourly_acc.pm4_0 / hourly_acc.count) : 0.0f,
                     hourly_acc.count ? (float)(hourly_acc.pm10_0 / hourly_acc.count) : 0.0f);
    httpd_resp_send_chunk(req, head, n);

    for (uint16_t i = 0; i < hourly_store.count; i++) {
        uint16_t idx = (hourly_store.head + HOURLY_RECORDS_LEN - hourly_store.count + i) %
                       HOURLY_RECORDS_LEN;
        const pm_hourly_record_t *r = &hourly_store.records[idx];
        char item[240];
        n = snprintf(item, sizeof(item),
                     "%s{\"seq\":%lu,\"start_ms\":%lld,\"end_ms\":%lld,"
                     "\"sample_count\":%lu,\"pm1_0\":%.2f,\"pm2_5\":%.2f,"
                     "\"pm4_0\":%.2f,\"pm10_0\":%.2f}",
                     i ? "," : "",
                     (unsigned long)r->seq,
                     (long long)r->start_ms,
                     (long long)r->end_ms,
                     (unsigned long)r->sample_count,
                     r->pm1_0,
                     r->pm2_5,
                     r->pm4_0,
                     r->pm10_0);
        if (n > 0) {
            httpd_resp_send_chunk(req, item, n);
        }
    }

    xSemaphoreGive(hourly_mutex);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t start_dashboard_httpd(void) {
    if (dashboard_httpd) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 5;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&dashboard_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP dashboard start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_root,
        .user_ctx = NULL,
    };
    const httpd_uri_t metrics = {
        .uri = "/api/metrics",
        .method = HTTP_GET,
        .handler = handler_metrics,
        .user_ctx = NULL,
    };
    const httpd_uri_t hist = {
        .uri = "/api/history",
        .method = HTTP_GET,
        .handler = handler_history,
        .user_ctx = NULL,
    };
    const httpd_uri_t hourly = {
        .uri = "/api/hourly",
        .method = HTTP_GET,
        .handler = handler_hourly,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(dashboard_httpd, &root);
    httpd_register_uri_handler(dashboard_httpd, &metrics);
    httpd_register_uri_handler(dashboard_httpd, &hist);
    httpd_register_uri_handler(dashboard_httpd, &hourly);

    ESP_LOGI(TAG, "HTTP dashboard listening on port %d", HTTP_PORT);
    return ESP_OK;
}

static void peer_warm_task(void *arg) {
    (void)arg;

    while (1) {
        if (ml && microlink_is_connected(ml)) {
            int peer_count = microlink_get_peer_count(ml);
            for (int i = 0; i < peer_count; i++) {
                microlink_peer_info_t info;
                if (microlink_get_peer_info(ml, i, &info) == ESP_OK && info.vpn_ip != 0) {
                    ml_wg_mgr_send_cmm(ml, info.vpn_ip);
                    ml_wg_mgr_trigger_handshake(ml, info.vpn_ip);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    msg_rx_count++;

    char ip_str[16];
    microlink_ip_to_str(src_ip, ip_str);

    char msg[256];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';
    if (copy_len > 0 && msg[copy_len - 1] == '\n') msg[copy_len - 1] = '\0';

    ESP_LOGI(TAG, "UDP RX #%lu from %s:%u [%d bytes]: \"%s\"",
             (unsigned long)msg_rx_count, ip_str, src_port, (int)len, msg);

    char reply[300];
    int reply_len = snprintf(reply, sizeof(reply), "ECHO: %s", msg);
    if (reply_len > 0) {
        esp_err_t err = microlink_udp_send(sock, src_ip, src_port, reply, reply_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UDP echo failed: %d", err);
        }
    }
}

static void ensure_udp_diag_socket(void) {
    if (!ml || udp_sock || !microlink_is_connected(ml)) {
        return;
    }

    udp_sock = microlink_udp_create(ml, MSG_PORT);
    if (!udp_sock) {
        ESP_LOGE(TAG, "Failed to create UDP diagnostic socket");
        return;
    }

    microlink_udp_set_rx_callback(udp_sock, on_udp_rx, NULL);
}

static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "MicroLink state: %s", ml_state_name(state));

    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "Connected. Dashboard: http://%s/  UDP diagnostics: %s:%d",
                 ip_str, ip_str, MSG_PORT);
        start_dashboard_httpd();
        ensure_udp_diag_socket();
        if (!peer_warm_task_handle) {
            xTaskCreatePinnedToCore(peer_warm_task, "peer_warm", 4096, NULL, 4,
                                    &peer_warm_task_handle, 1);
        }
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                           void *user_data) {
    (void)ml_handle;
    (void)user_data;
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}

static void wifi_try_next(void) {
    if (wifi_list_count <= 1) {
        esp_wifi_connect();
        return;
    }

    wifi_retry_count++;
    if (wifi_retry_count >= WIFI_MAX_RETRIES_PER_SSID) {
        wifi_retry_count = 0;
        current_wifi_idx = (current_wifi_idx + 1) % wifi_list_count;
    }

    ml_config_wifi_entry_t *e = &wifi_list.entries[current_wifi_idx];
    wifi_config_t wifi_config = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    strncpy((char *)wifi_config.sta.ssid, e->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, e->pass, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "WiFi trying #%d/%d: %s", current_wifi_idx + 1, wifi_list_count, e->ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_try_next();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi_ip_str, sizeof(wifi_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected to %s, IP: " IPSTR,
                 wifi_list_count > 0 ? wifi_list.entries[current_wifi_idx].ssid : wifi_ssid,
                 IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi init complete, connecting to %s", wifi_ssid);
}

void app_main(void) {
    board_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    hourly_store_init();

    sensor_mutex = xSemaphoreCreateMutex();
    history = heap_caps_calloc(SENSOR_HISTORY_LEN, sizeof(pm_sps30_sample_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!history) {
        history = calloc(SENSOR_HISTORY_LEN, sizeof(pm_sps30_sample_t));
    }
    if (!history) {
        ESP_LOGE(TAG, "Failed to allocate sensor history");
    } else {
        ESP_LOGI(TAG, "Sensor history allocated: %d samples (%d seconds)",
                 SENSOR_HISTORY_LEN, (SENSOR_HISTORY_LEN * SENSOR_INTERVAL_MS) / 1000);
    }
    xTaskCreatePinnedToCore(sensor_task, "pm_sensor", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "PM MicroLink dashboard firmware");
    ESP_LOGI(TAG, "Free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    memset(&wifi_list, 0, sizeof(wifi_list));
    wifi_list.active_idx = 0xFF;

    if (ml_config_get_wifi_list(&wifi_list) && wifi_list.count > 1) {
        wifi_list_count = wifi_list.count;
        current_wifi_idx = 0;
        strncpy(wifi_ssid, wifi_list.entries[0].ssid, sizeof(wifi_ssid) - 1);
        strncpy(wifi_password, wifi_list.entries[0].pass, sizeof(wifi_password) - 1);
        ESP_LOGI(TAG, "WiFi multi-SSID: %d networks", wifi_list_count);
    } else if (ml_config_get_nvs_wifi(wifi_ssid, sizeof(wifi_ssid),
                                      wifi_password, sizeof(wifi_password))) {
        ESP_LOGI(TAG, "Using NVS WiFi: %s", wifi_ssid);
    } else {
        ESP_LOGI(TAG, "Using Kconfig WiFi: %s", wifi_ssid);
    }

    wifi_init();
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    start_dashboard_httpd();
    ESP_LOGI(TAG, "WiFi dashboard: http://%s/ (Tailscale dashboard will use port 80 after VPN connects)",
             wifi_ip_str);

    microlink_config_t config = {
        .auth_key = PM_TAILSCALE_AUTH_KEY,
        .device_name = PM_DEVICE_NAME[0] ? PM_DEVICE_NAME : "pm-microlink",
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 13,
        .priority_peer_ip = microlink_parse_ip(CONFIG_ML_PRIORITY_PEER_IP),
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "Failed to initialize MicroLink");
    } else {
        microlink_set_state_callback(ml, on_state_change, NULL);
        microlink_set_peer_callback(ml, on_peer_update, NULL);
        ESP_ERROR_CHECK(microlink_start(ml));
    }

    uint32_t target_ip = 0;
    const char *target_ip_str = CONFIG_ML_EXAMPLE_TARGET_PEER_IP;
    if (target_ip_str && target_ip_str[0] != '\0') {
        target_ip = microlink_parse_ip(target_ip_str);
    }

    uint64_t last_send_ms = 0;
    uint64_t last_status_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t now = (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        ensure_udp_diag_socket();

        if (udp_sock && target_ip != 0 && now - last_send_ms >= MSG_SEND_INTERVAL_MS) {
            last_send_ms = now;
            msg_tx_count++;
            char msg[128];
            int msg_len = snprintf(msg, sizeof(msg), "hello from ESP32 #%lu",
                                   (unsigned long)msg_tx_count);
            microlink_udp_send(udp_sock, target_ip, MSG_PORT, msg, msg_len);
        }

        if (now - last_status_ms >= 30000) {
            last_status_ms = now;
            pm_sps30_status_t st;
            pm_sps30_get_status(&st);
            ESP_LOGI(TAG, "Status: ml=%s peers=%d sensor=%s reads=%lu errors=%lu heap=%lu",
                     ml ? ml_state_name(microlink_get_state(ml)) : "offline",
                     ml ? microlink_get_peer_count(ml) : 0,
                     pm_sps30_state_name(st.state),
                     (unsigned long)st.read_count,
                     (unsigned long)st.error_count,
                     (unsigned long)esp_get_free_heap_size());
        }
    }
}
