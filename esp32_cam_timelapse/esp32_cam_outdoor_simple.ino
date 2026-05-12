/*
  ESP32-CAM 户外定时拍照装置 v2.0 (简化版)

  核心功能：
  - 定时拍照（可配置间隔）
  - 太阳能 + 锂电池供电
  - 夜视自动切换
  - 电池电量监测
  - 深度睡眠节能
  - WiFi远程查看

  适用：延时摄影、自然观察、户外监测
*/

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "esp_http_server.h"
#include "esp_sleep.h"
#include "WiFi.h"
#include "driver/adc.h"

// ===================
// 配置区域
// ===================

// WiFi配置
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// 拍照配置
const unsigned long CAPTURE_INTERVAL = 60000;  // 拍照间隔（毫秒），默认60秒
const uint64_t DEEP_SLEEP_TIME_US = 60 * 1000000ULL;  // 深度睡眠时间（微秒）

// 夜视配置
const bool ENABLE_NIGHT_VISION = true;
const int LIGHT_SENSOR_PIN = 34;  // 光敏电阻 ADC 引脚
const int LIGHT_THRESHOLD = 500;  // 光线阈值（低于此值开启夜视）

// 红外LED配置
const int IR_LED_PIN = 2;

// 照片配置
const int JPEG_QUALITY = 12;
const framesize_t FRAME_SIZE = FRAMESIZE_SVGA;  // 800x600

// Web服务器
const bool ENABLE_WEB_SERVER = true;

// 电池监测
const float BATTERY_MAX_VOLTAGE = 8.4;  // 双节18650满电
const float BATTERY_MIN_VOLTAGE = 6.0;   // 双节18650保护电压
const int MIN_BATTERY_PERCENT = 10;      // 低于此电量进入深度睡眠

// ===================
// ESP32-CAM 引脚定义
// ===================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ===================
// 全局变量
// ===================
int photoCount = 0;
int dayPhotoCount = 0;
int nightPhotoCount = 0;
bool sdCardAvailable = false;
bool isNight = false;
httpd_handle_t camera_httpd = NULL;

// ===================
// 函数声明
// ===================
bool initCamera();
bool initSDCard();
String getTimestamp();
bool captureAndSavePhoto();
float readBatteryVoltage();
int getBatteryPercentage();
bool isNightTime();
void goToSleep();
void startWebServer();

// ===================
// 初始化
// ===================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();
  Serial.println("=== ESP32-CAM 户外定时拍照装置 v2.0 ===");
  Serial.println("模式: 纯定时拍照 + 太阳能供电");

  // 初始化光敏电阻
  pinMode(LIGHT_SENSOR_PIN, INPUT);
  delay(100);

  // 读取电池电量
  float voltage = readBatteryVoltage();
  int batteryPct = getBatteryPercentage();
  Serial.printf("电池电压: %.2fV (%d%%)\n", voltage, batteryPct);

  // 电量不足进入深度睡眠
  if (batteryPct < MIN_BATTERY_PERCENT) {
    Serial.println("电池电量过低，进入深度睡眠省电...");
    goToSleep();
  }

  // 判断是否夜间
  isNight = isNightTime();
  Serial.printf("环境光线: %s\n", isNight ? "夜间" : "白天");

  // 设置红外LED
  if (ENABLE_NIGHT_VISION) {
    pinMode(IR_LED_PIN, OUTPUT);
    digitalWrite(IR_LED_PIN, isNight ? HIGH : LOW);
  }

  // 初始化摄像头
  if (!initCamera()) {
    Serial.println("摄像头初始化失败!");
    delay(1000);
    goToSleep();
  }

  // 初始化SD卡
  sdCardAvailable = initSDCard();

  // 连接WiFi
  if (ENABLE_WEB_SERVER) {
    Serial.println("正在连接WiFi...");
    WiFi.begin(ssid, password);

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 15) {
      delay(500);
      Serial.print(".");
      retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi已连接!");
      Serial.print("IP地址: ");
      Serial.println(WiFi.localIP());
      startWebServer();
    } else {
      Serial.println("\nWiFi连接失败，继续离线模式");
    }
  }

  Serial.printf("\n系统就绪！拍照间隔: %lu 秒\n", CAPTURE_INTERVAL / 1000);
  Serial.printf("累计照片: %d (白天: %d / 夜间: %d)\n", photoCount, dayPhotoCount, nightPhotoCount);
  Serial.println("==========================================");
}

// ===================
// 主循环 - 拍照一次后进入睡眠
// ===================
void loop() {
  // 拍照并保存
  if (captureAndSavePhoto()) {
    photoCount++;
    if (isNight) {
      nightPhotoCount++;
    } else {
      dayPhotoCount++;
    }
    Serial.printf("拍照成功! [%d] 累计: %d\n",
                  isNight ? nightPhotoCount : dayPhotoCount,
                  photoCount);
  }

  delay(2000);

  Serial.println("进入深度睡眠...");
  goToSleep();
}

// ===================
// 读取电池电压
// ===================
float readBatteryVoltage() {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

  int sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += adc1_get_raw(ADC1_CHANNEL_6);
    delay(10);
  }
  int avg = sum / 10;

  // 分压电阻: 100K + 100K, 满压8.4V对应ADC约2480
  float voltage = avg / 4095.0 * 3.3 * 2;
  return voltage;
}

// ===================
// 获取电池百分比
// ===================
int getBatteryPercentage() {
  float voltage = readBatteryVoltage();
  float percentage = (voltage - BATTERY_MIN_VOLTAGE) /
                     (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE) * 100;
  percentage = constrain(percentage, 0, 100);
  return (int)percentage;
}

// ===================
// 判断是否夜间
// ===================
bool isNightTime() {
  if (!ENABLE_NIGHT_VISION) return false;

  int lightLevel = analogRead(LIGHT_SENSOR_PIN);
  Serial.printf("光线ADC值: %d (阈值: %d)\n", lightLevel, LIGHT_THRESHOLD);
  return lightLevel < LIGHT_THRESHOLD;
}

// ===================
// 进入深度睡眠
// ===================
void goToSleep() {
  Serial.println("正在进入深度睡眠...");

  // 关闭红外LED
  if (ENABLE_NIGHT_VISION) {
    digitalWrite(IR_LED_PIN, LOW);
  }

  // 关闭摄像头
  esp_camera_deinit();

  // 关闭WiFi
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  // 配置定时唤醒
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME_US);

  // 进入深度睡眠
  esp_deep_sleep_start();
}

// ===================
// 初始化摄像头
// ===================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败: 0x%x\n", err);
    return false;
  }

  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    if (isNight) {
      s->set_brightness(s, 1);
      s->set_contrast(s, 1);
    }
  }

  Serial.println("摄像头初始化成功");
  return true;
}

// ===================
// 初始化SD卡
// ===================
bool initSDCard() {
  Serial.println("正在初始化SD卡...");

  if (!SD_MMC.begin()) {
    Serial.println("SD卡挂载失败!");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("未检测到SD卡!");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD卡容量: %llu MB\n", cardSize);

  if (!SD_MMC.exists("/photos")) {
    SD_MMC.mkdir("/photos");
  }

  if (!SD_MMC.exists("/photos/day")) {
    SD_MMC.mkdir("/photos/day");
  }

  if (!SD_MMC.exists("/photos/night")) {
    SD_MMC.mkdir("/photos/night");
  }

  Serial.println("SD卡初始化成功");
  return true;
}

// ===================
// 获取时间戳
// ===================
String getTimestamp() {
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  char buf[32];
  sprintf(buf, "%02luh%02um%02us_%03lu",
          hours, minutes % 60, seconds % 60, ms % 1000);
  return String(buf);
}

// ===================
// 拍照并保存
// ===================
bool captureAndSavePhoto() {
  // 夜视LED亮起
  if (ENABLE_NIGHT_VISION) {
    digitalWrite(IR_LED_PIN, HIGH);
    delay(100);
  }

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("拍照失败!");
    if (ENABLE_NIGHT_VISION) digitalWrite(IR_LED_PIN, LOW);
    return false;
  }

  String timestamp = getTimestamp();
  String folder = isNight ? "/photos/night" : "/photos/day";
  String filename = folder + "/photo_" + timestamp + ".jpg";

  bool success = false;
  if (sdCardAvailable) {
    File file = SD_MMC.open(filename.c_str(), FILE_WRITE);
    if (file) {
      file.write(fb->buf, fb->len);
      file.close();
      Serial.printf("已保存: %s (%d KB)\n", filename.c_str(), fb->len / 1024);
      success = true;
    } else {
      Serial.println("创建文件失败!");
    }
  }

  esp_camera_fb_return(fb);

  // 夜视LED熄灭
  if (ENABLE_NIGHT_VISION) {
    digitalWrite(IR_LED_PIN, LOW);
  }

  return success;
}

// ===================
// Web服务器
// ===================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM 户外拍照</title>
    <style>
        * { box-sizing: border-box; }
        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%); color: #fff; min-height: 100vh; }
        .container { max-width: 900px; margin: 0 auto; }
        h1 { text-align: center; color: #00d4ff; margin-bottom: 30px; font-size: 28px; }
        .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin-bottom: 30px; }
        .status-card { background: rgba(255,255,255,0.1); padding: 20px; border-radius: 15px; text-align: center; backdrop-filter: blur(10px); }
        .status-card h3 { margin: 0 0 10px 0; color: #00d4ff; font-size: 14px; text-transform: uppercase; }
        .status-card .value { font-size: 32px; font-weight: bold; color: #fff; }
        .status-card .unit { font-size: 14px; color: #aaa; }
        .battery-container { margin-top: 10px; }
        .battery-bar { width: 100%; height: 20px; background: #333; border-radius: 10px; overflow: hidden; }
        .battery-fill { height: 100%; background: linear-gradient(90deg, #00ff00, #88ff00); transition: width 0.5s; border-radius: 10px; }
        .battery-low .battery-fill { background: linear-gradient(90deg, #ff4444, #ff0000); }
        .buttons { display: flex; gap: 15px; justify-content: center; flex-wrap: wrap; margin: 30px 0; }
        .btn { padding: 15px 30px; border: none; border-radius: 25px; font-size: 16px; cursor: pointer; text-decoration: none; display: inline-block; transition: all 0.3s; font-weight: bold; }
        .btn-primary { background: #00d4ff; color: #1a1a2e; }
        .btn-primary:hover { background: #00a8cc; transform: scale(1.05); }
        .btn-secondary { background: #4a4a6a; color: #fff; }
        .btn-secondary:hover { background: #5a5a7a; transform: scale(1.05); }
        .btn-danger { background: #e94560; color: #fff; }
        .btn-danger:hover { background: #d13050; transform: scale(1.05); }
        .photo-list { background: rgba(255,255,255,0.05); border-radius: 15px; padding: 20px; margin-top: 30px; }
        .photo-list h2 { margin-top: 0; color: #00d4ff; }
        .photo-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 15px; }
        .photo-item { background: rgba(0,0,0,0.3); padding: 10px; border-radius: 10px; text-align: center; }
        .photo-item img { width: 100%; border-radius: 5px; }
        .photo-item .name { margin-top: 8px; font-size: 12px; color: #aaa; word-break: break-all; }
        .loading { text-align: center; padding: 20px; color: #888; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📷 ESP32-CAM 户外定时拍照</h1>

        <div class="status-grid">
            <div class="status-card">
                <h3>🔋 电池电量</h3>
                <div class="battery-container">
                    <div class="battery-bar">
                        <div class="battery-fill" id="batteryFill"></div>
                    </div>
                    <span id="batteryPct">--</span>
                </div>
            </div>
            <div class="status-card">
                <h3>🌙 当前状态</h3>
                <div class="value" id="mode">--</div>
            </div>
            <div class="status-card">
                <h3>☀️ 白天照片</h3>
                <div class="value" id="dayCount">0</div>
            </div>
            <div class="status-card">
                <h3>🌃 夜间照片</h3>
                <div class="value" id="nightCount">0</div>
            </div>
        </div>

        <div class="buttons">
            <a href="/capture" class="btn btn-primary" target="_blank">📷 立即拍照</a>
            <a href="/list" class="btn btn-secondary">📁 照片列表</a>
            <a href="/wake" class="btn btn-secondary">🔄 唤醒查看</a>
            <a href="/sleep" class="btn btn-danger">😴 深度睡眠</a>
        </div>
    </div>

    <script>
        function loadStatus() {
            document.getElementById('mode').textContent = '正在获取...';
        }
        loadStatus();
    </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
  return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>照片列表</title>
    <style>
        body { font-family: Arial; margin: 20px; background: #1a1a2e; color: #fff; }
        .container { max-width: 1000px; margin: 0 auto; }
        h1 { color: #00d4ff; }
        .nav { margin-bottom: 20px; }
        a { color: #00d4ff; text-decoration: none; padding: 10px 20px; display: inline-block; background: #16213e; border-radius: 20px; }
        a:hover { background: #00d4ff; color: #1a1a2e; }
        .section { margin: 30px 0; }
        .section h2 { color: #00d4ff; border-bottom: 2px solid #00d4ff; padding-bottom: 10px; }
        .photos { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); gap: 15px; }
        .photo { background: #16213e; padding: 10px; border-radius: 10px; }
        .photo img { width: 100%; border-radius: 5px; }
        .photo .info { margin-top: 8px; font-size: 12px; color: #aaa; }
        .photo .actions { margin-top: 8px; }
        .photo .actions a { padding: 5px 15px; font-size: 12px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📁 照片列表</h1>
        <div class="nav">
            <a href="/">← 返回首页</a>
        </div>
)rawliteral";

  if (sdCardAvailable) {
    // 白天照片
    html += "<div class='section'><h2>☀️ 白天照片</h2><div class='photos'>";
    File dayFolder = SD_MMC.open("/photos/day");
    if (dayFolder && dayFolder.isDirectory()) {
      File file = dayFolder.openNextFile();
      while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".jpg")) {
          html += "<div class='photo'>";
          html += "<img src='/download?file=day/" + String(file.name()) + "'>";
          html += "<div class='info'>" + String(file.name()) + " (" + String(file.size()/1024) + " KB)</div>";
          html += "<div class='actions'><a href='/download?file=day/" + String(file.name()) + "'>下载</a></div>";
          html += "</div>";
        }
        file = dayFolder.openNextFile();
      }
    }
    html += "</div></div>";

    // 夜间照片
    html += "<div class='section'><h2>🌃 夜间照片</h2><div class='photos'>";
    File nightFolder = SD_MMC.open("/photos/night");
    if (nightFolder && nightFolder.isDirectory()) {
      File file = nightFolder.openNextFile();
      while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".jpg")) {
          html += "<div class='photo'>";
          html += "<img src='/download?file=night/" + String(file.name()) + "'>";
          html += "<div class='info'>" + String(file.name()) + " (" + String(file.size()/1024) + " KB)</div>";
          html += "<div class='actions'><a href='/download?file=night/" + String(file.name()) + "'>下载</a></div>";
          html += "</div>";
        }
        file = nightFolder.openNextFile();
      }
    }
    html += "</div></div>";
  } else {
    html += "<p>SD卡不可用</p>";
  }

  html += "</div></body></html>";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req) {
  char buf[128];
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;

  if (buf_len > 1) {
    char *query = (char*)malloc(buf_len);
    httpd_req_get_url_query_str(req, query, buf_len);

    char filename[64];
    if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK) {
      String path = "/photos/" + String(filename);
      if (SD_MMC.exists(path)) {
        File file = SD_MMC.open(path, FILE_READ);
        if (file) {
          httpd_resp_set_type(req, "image/jpeg");
          uint8_t buffer[1024];
          while (file.available()) {
            size_t read_len = file.read(buffer, 1024);
            httpd_resp_send_chunk(req, (const char*)buffer, read_len);
          }
          httpd_resp_send_chunk(req, NULL, 0);
          file.close();
          free(query);
          return ESP_OK;
        }
      }
    }
    free(query);
  }

  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t sleep_handler(httpd_req_t *req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>进入睡眠</title>
    <style>
        body { font-family: Arial; margin: 0; padding: 0; background: #1a1a2e; color: #fff; display: flex; justify-content: center; align-items: center; height: 100vh; }
        .message { text-align: center; }
        h1 { color: #00d4ff; }
        p { color: #aaa; }
    </style>
</head>
<body>
    <div class="message">
        <h1>😴 即将进入深度睡眠</h1>
        <p>设备将在设定时间后自动唤醒...</p>
        <p>刷新页面可唤醒设备</p>
    </div>
    <script>setTimeout(() => location.href = '/', 3000);</script>
</body>
</html>
)rawliteral";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());

  delay(500);
  goToSleep();

  return ESP_OK;
}

static esp_err_t wake_handler(httpd_req_t *req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>唤醒设备</title>
    <style>
        body { font-family: Arial; margin: 0; padding: 0; background: #1a1a2e; color: #fff; display: flex; justify-content: center; align-items: center; height: 100vh; }
        .message { text-align: center; }
        h1 { color: #00ff00; }
    </style>
</head>
<body>
    <div class="message">
        <h1>✓ 设备已唤醒</h1>
        <p>正在重新连接...</p>
    </div>
    <script>setTimeout(() => location.href = '/', 2000);</script>
</body>
</html>
)rawliteral";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());

  return ESP_OK;
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t list_uri = { .uri = "/list", .method = HTTP_GET, .handler = list_handler, .user_ctx = NULL };
  httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_handler, .user_ctx = NULL };
  httpd_uri_t sleep_uri = { .uri = "/sleep", .method = HTTP_GET, .handler = sleep_handler, .user_ctx = NULL };
  httpd_uri_t wake_uri = { .uri = "/wake", .method = HTTP_GET, .handler = wake_handler, .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &list_uri);
    httpd_register_uri_handler(camera_httpd, &download_uri);
    httpd_register_uri_handler(camera_httpd, &sleep_uri);
    httpd_register_uri_handler(camera_httpd, &wake_uri);
    Serial.println("Web服务器已启动");
  }
}
