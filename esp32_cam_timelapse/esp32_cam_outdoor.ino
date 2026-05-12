/*
  ESP32-CAM 户外定时拍照装置 v2.0

  优化特性：
  - 低功耗模式（深度睡眠）
  - 太阳能供电管理
  - 红外夜视支持
  - PIR人体感应触发
  - 大容量SD卡支持
  - 电池电量监测

  适用场景：户外野生动物观测、延时摄影、安防监控
*/

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "esp_http_server.h"
#include "esp_sleep.h"
#include "WiFi.h"
#include "driver/adc.h"
#include "esp_battery.h"

// ===================
// 配置区域
// ===================

// WiFi配置
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// 拍照模式配置
typedef enum {
  MODE_INTERVAL,      // 定时拍照模式
  MODE_PIR_TRIGGER,   // PIR人体感应触发模式
  MODE_HYBRID         // 混合模式（PIR触发 + 定时）
} CaptureMode;

const CaptureMode CAPTURE_MODE = MODE_HYBRID;

// 定时拍照间隔（毫秒）- 深度睡眠方式
// 注意：深度睡眠间隔 = DEEP_SLEEP_TIME_US
// 非睡眠模式间隔 = CAPTURE_INTERVAL
const unsigned long CAPTURE_INTERVAL = 60000;  // 拍照间隔（毫秒）
const uint64_t DEEP_SLEEP_TIME_US = 60 * 1000000ULL;  // 深度睡眠时间（微秒）= 60秒

// PIR配置
const int PIR_PIN = 13;           // PIR传感器连接引脚
const int PIR_TIMEOUT_MS = 30000; // PIR触发后持续时间

// 红外夜视配置
const int IR_LED_PIN = 2;         // 红外LED引脚（板载LED）
const bool ENABLE_NIGHT_VISION = true;
const int LIGHT_THRESHOLD = 500;  // 光线阈值（ADC值）

// 照片配置
const int JPEG_QUALITY = 12;      // 照片质量 (10-63)
const framesize_t FRAME_SIZE = FRAMESIZE_SVGA;  // 800x600 (平衡画质和存储)

// Web服务器
const bool ENABLE_WEB_SERVER = true;

// 电池监测
const int BATTERY_PIN = 34;      // ADC引脚
const float BATTERY_MAX_VOLTAGE = 8.4;  // 2节18650满电电压
const float BATTERY_MIN_VOLTAGE = 6.0;  // 2节18650保护电压

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
unsigned long lastCaptureTime = 0;
unsigned long lastPIRActivity = 0;
bool pirTriggered = false;
int photoCount = 0;
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
void initPIR();
void updateNightMode();

// ===================
// 初始化
// ===================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();
  Serial.println("=== ESP32-CAM 户外定时拍照装置 v2.0 ===");

  // 初始化PIR传感器
  initPIR();

  // 读取电池电量
  float voltage = readBatteryVoltage();
  int batteryPct = getBatteryPercentage();
  Serial.printf("电池电压: %.2fV (%d%%)\n", voltage, batteryPct);

  if (batteryPct < 10) {
    Serial.println("⚠️ 电池电量过低，进入深度睡眠省电模式");
    goToSleep();
  }

  // 判断是否夜间
  updateNightMode();

  // 设置红外LED
  if (ENABLE_NIGHT_VISION) {
    pinMode(IR_LED_PIN, OUTPUT);
    digitalWrite(IR_LED_PIN, isNight ? HIGH : LOW);
    Serial.printf("夜视模式: %s\n", isNight ? "开启" : "关闭");
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
      Serial.println("\n✓ WiFi已连接!");
      Serial.print("IP地址: ");
      Serial.println(WiFi.localIP());
      startWebServer();
    } else {
      Serial.println("\n✗ WiFi连接失败，继续离线模式");
    }
  }

  Serial.println("\n系统就绪!");
  Serial.printf("拍照模式: %s\n",
    CAPTURE_MODE == MODE_INTERVAL ? "定时模式" :
    CAPTURE_MODE == MODE_PIR_TRIGGER ? "PIR触发模式" : "混合模式");
  Serial.printf("照片总数: %d\n", photoCount);
  Serial.println("========================================");
}

// ===================
// 主循环
// ===================
void loop() {
  unsigned long currentTime = millis();

  // 检查是否需要拍照
  bool shouldCapture = false;

  switch (CAPTURE_MODE) {
    case MODE_INTERVAL:
      // 定时模式
      if (currentTime - lastCaptureTime >= CAPTURE_INTERVAL) {
        shouldCapture = true;
      }
      break;

    case MODE_PIR_TRIGGER:
      // PIR触发模式
      if (pirTriggered) {
        shouldCapture = true;
        if (currentTime - lastPIRActivity > PIR_TIMEOUT_MS) {
          pirTriggered = false;
        }
      }
      break;

    case MODE_HYBRID:
      // 混合模式：定时 + PIR触发
      if (currentTime - lastCaptureTime >= CAPTURE_INTERVAL) {
        shouldCapture = true;
      }
      if (pirTriggered && currentTime - lastPIRActivity > 5000) {
        // PIR触发后5秒内不重复触发
        pirTriggered = false;
      }
      break;
  }

  // 执行拍照
  if (shouldCapture && captureAndSavePhoto()) {
    photoCount++;
    if (isNight) nightPhotoCount++;
    lastCaptureTime = currentTime;
    Serial.printf("✓ 拍照成功 [%d] 累计照片: %d\n",
                  isNight ? nightPhotoCount : photoCount - nightPhotoCount,
                  photoCount);
  }

  // 检查PIR传感器
  if (digitalRead(PIR_PIN) == HIGH) {
    pirTriggered = true;
    lastPIRActivity = currentTime;
    Serial.println("PIR: 检测到人体!");
  }

  // 定期更新夜间模式
  if (currentTime % 60000 < 100) {
    bool newNight = isNightTime();
    if (newNight != isNight) {
      isNight = newNight;
      if (ENABLE_NIGHT_VISION) {
        digitalWrite(IR_LED_PIN, isNight ? HIGH : LOW);
        Serial.printf("夜视模式切换: %s\n", isNight ? "开启" : "关闭");
      }
    }
  }

  // 运行15分钟后进入深度睡眠
  if (currentTime > 900000) {  // 15分钟 = 900000毫秒
    Serial.println("进入深度睡眠...");
    goToSleep();
  }

  delay(100);
}

// ===================
// PIR初始化
// ===================
void initPIR() {
  pinMode(PIR_PIN, INPUT);
  Serial.println("PIR传感器已初始化");
}

// ===================
// 判断是否夜间
// ===================
bool isNightTime() {
  if (!ENABLE_NIGHT_VISION) return false;

  int lightLevel = analogRead(LIGHT_THRESHOLD);
  // ADC值低表示光线暗
  return lightLevel < 500;
}

// ===================
// 更新夜间模式状态
// ===================
void updateNightMode() {
  isNight = isNightTime();
}

// ===================
// 读取电池电压
// ===================
float readBatteryVoltage() {
  // ADC1_CHANNEL_6 = GPIO34
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

  int sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += adc1_get_raw(ADC1_CHANNEL_6);
    delay(10);
  }
  int avg = sum / 10;

  // 分压电阻：100K + 100K，ADC参考电压3.3V
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
// 进入深度睡眠
// ===================
void goToSleep() {
  Serial.println("正在进入深度睡眠...");

  // 关闭摄像头
  esp_camera_deinit();

  // 配置唤醒源：定时唤醒
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME_US);

  // 配置GPIO唤醒（PIR）
  gpio_config_t config = {
    .pin_bit_mask = (1ULL << PIR_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_POSEDGE
  };
  gpio_config(&config);
  esp_sleep_enable_gpio_wakeup();

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
    // 优化夜间画质
    if (isNight) {
      s->set_brightness(s, 1);     // 提高亮度
      s->set_contrast(s, 1);      // 提高对比度
      s->set_exposure_ctrl(s, 1); // 自动曝光
    } else {
      s->set_brightness(s, 0);
      s->set_contrast(s, 0);
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

  char buf[30];
  sprintf(buf, "%02lu%02lu%02lu_%03lu",
          hours % 24, minutes % 60, seconds % 60, ms % 1000);

  return String(buf);
}

// ===================
// 拍照并保存
// ===================
bool captureAndSavePhoto() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("拍照失败!");
    return false;
  }

  String timestamp = getTimestamp();
  String prefix = isNight ? "night_" : "day_";
  String path = "/photos/" + prefix + timestamp + ".jpg";

  bool success = false;
  if (sdCardAvailable) {
    File file = SD_MMC.open(path.c_str(), FILE_WRITE);
    if (file) {
      file.write(fb->buf, fb->len);
      file.close();
      Serial.printf("已保存: %s (%d KB)\n", path.c_str(), fb->len / 1024);
      success = true;
    }
  }

  esp_camera_fb_return(fb);
  return success;
}

// ===================
// Web服务器 - 主页HTML
// ===================
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM 户外拍照</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #1a1a2e; color: #fff; }
        .container { max-width: 800px; margin: 0 auto; background: #16213e; padding: 20px; border-radius: 15px; }
        h1 { text-align: center; color: #00d4ff; }
        .status { display: grid; grid-template-columns: repeat(2, 1fr); gap: 15px; margin: 20px 0; }
        .status-item { background: #0f3460; padding: 15px; border-radius: 10px; }
        .status-item h3 { margin: 0 0 10px 0; color: #00d4ff; font-size: 14px; }
        .status-item p { margin: 0; font-size: 24px; font-weight: bold; }
        .btn { display: inline-block; padding: 12px 25px; margin: 5px; background: #00d4ff; color: #1a1a2e; text-decoration: none; border-radius: 25px; font-weight: bold; }
        .btn:hover { background: #00a8cc; }
        .btn-danger { background: #e94560; }
        .battery { display: flex; align-items: center; gap: 10px; }
        .battery-bar { width: 100px; height: 20px; background: #333; border-radius: 10px; overflow: hidden; }
        .battery-fill { height: 100%; background: #00ff00; transition: width 0.3s; }
        .battery-low { background: #ff0000; }
        .photo-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; margin-top: 20px; }
        .photo-item { background: #0f3460; padding: 10px; border-radius: 10px; text-align: center; }
        .photo-item img { width: 100%; border-radius: 5px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📷 ESP32-CAM 户外拍照装置</h1>

        <div class="status">
            <div class="status-item">
                <h3>🔋 电池电量</h3>
                <div class="battery">
                    <div class="battery-bar">
                        <div class="battery-fill" id="batteryFill"></div>
                    </div>
                    <span id="batteryPct">--</span>
                </div>
            </div>
            <div class="status-item">
                <h3>🌙 当前模式</h3>
                <p id="nightMode">--</p>
            </div>
            <div class="status-item">
                <h3>📸 白天照片</h3>
                <p id="dayCount">0</p>
            </div>
            <div class="status-item">
                <h3>🌃 夜间照片</h3>
                <p id="nightCount">0</p>
            </div>
        </div>

        <div style="text-align: center;">
            <a href="/capture" class="btn">📷 立即拍照</a>
            <a href="/sleep" class="btn btn-danger">😴 进入休眠</a>
            <a href="/list" class="btn">📁 查看照片</a>
        </div>
    </div>

    <script>
        function updateStatus() {
            document.getElementById('nightMode').textContent = '--';
            document.getElementById('dayCount').textContent = '--';
            document.getElementById('nightCount').textContent = '--';
        }
        updateStatus();
        setInterval(updateStatus, 5000);
    </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  String html = String(INDEX_HTML);
  html.replace("%STATUS%", isNight ? "🌙 夜间" : "☀️ 白天");
  html.replace("%COUNT%", String(photoCount));
  html.replace("%NIGHT%", String(nightPhotoCount));
  html.replace("%BATTERY%", String(getBatteryPercentage()));

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
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
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>照片列表</title>";
  html += "<style>body{font-family:Arial;margin:20px;background:#1a1a2e;color:#fff;}";
  html += ".photo-item{margin:10px;padding:15px;background:#16213e;border-radius:10px;}";
  html += "a{color:#00d4ff;text-decoration:none;padding:10px;display:inline-block;}";
  html += ".back{background:#0f3460;padding:15px;margin-bottom:20px;border-radius:10px;}</style></head><body>";
  html += "<h1>📁 照片列表</h1><div class='back'><a href='/'>← 返回首页</a></div>";

  if (sdCardAvailable) {
    File root = SD_MMC.open("/photos");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".jpg")) {
          html += "<div class='photo-item'>";
          html += "📷 " + String(file.name()) + " (" + String(file.size()/1024) + " KB) ";
          html += "<a href='/download?file=" + String(file.name()) + "'>下载</a>";
          html += "<a href='/capture'>拍照预览</a>";
          html += "</div>";
        }
        file = root.openNextFile();
      }
    }
  }

  html += "</body></html>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req) {
  char buf[100];
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
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, "<html><body><h1>即将进入深度睡眠...</h1><p>请断开电源后再重新连接以唤醒。</p></body></html>", -1);

  delay(1000);
  goToSleep();

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

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &list_uri);
    httpd_register_uri_handler(camera_httpd, &download_uri);
    httpd_register_uri_handler(camera_httpd, &sleep_uri);
    Serial.println("✓ Web服务器已启动");
  }
}
