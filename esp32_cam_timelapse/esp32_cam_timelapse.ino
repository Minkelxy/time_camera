/*
  ESP32-CAM 定时拍照装置
  
  硬件: ESP32-CAM (AI-Thinker 模块)
  功能: 定时拍照并保存到SD卡，支持通过Web界面查看和下载
  
  接线说明:
  - 使用USB-TTL模块连接:
    ESP32-CAM    USB-TTL
    GND    -->   GND
    5V     -->   5V
    U0R    -->   TXD
    U0T    -->   RXD
    GPIO0  -->   GND (下载模式，下载完成后断开)
  
  引脚定义:
  - GPIO 4: SD卡 CS
  - GPIO 2: SD卡 MISO
  - GPIO 14: SD卡 CLK
  - GPIO 15: SD卡 MOSI
  - GPIO 0: 下载模式控制
*/

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "esp_http_server.h"
#include "WiFi.h"
#include "time.h"

// ===================
// 配置区域 - 根据你的网络修改
// ===================
const char* ssid = "YOUR_WIFI_SSID";        // WiFi名称
const char* password = "YOUR_WIFI_PASSWORD"; // WiFi密码

// 定时拍照间隔（毫秒）
const unsigned long CAPTURE_INTERVAL = 60000;  // 默认60秒

// 是否启用Web服务器
const bool ENABLE_WEB_SERVER = true;

// 照片质量 (10-63, 越小质量越好)
const int JPEG_QUALITY = 10;

// 分辨率设置
// FRAMESIZE_96X96, FRAMESIZE_QQVGA(160x120), FRAMESIZE_QCIF(176x144)
// FRAMESIZE_HQVGA(240x176), FRAMESIZE_240X240, FRAMESIZE_QVGA(320x240)
// FRAMESIZE_CIF(400x296), FRAMESIZE_HVGA(480x320), FRAMESIZE_VGA(640x480)
// FRAMESIZE_SVGA(800x600), FRAMESIZE_XGA(1024x768), FRAMESIZE_HD(1280x720)
// FRAMESIZE_SXGA(1280x1024), FRAMESIZE_UXGA(1600x1200)
const framesize_t FRAME_SIZE = FRAMESIZE_VGA;  // 640x480

// ===================
// ESP32-CAM 引脚定义 (AI-Thinker 模块)
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
#define LED_GPIO_NUM       4  // 板载LED（也是SD卡CS）

// ===================
// 全局变量
// ===================
unsigned long lastCaptureTime = 0;
int photoCount = 0;
bool sdCardAvailable = false;
httpd_handle_t camera_httpd = NULL;

// ===================
// 函数声明
// ===================
bool initCamera();
bool initSDCard();
String getTimestamp();
bool captureAndSavePhoto();
void startWebServer();
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t capture_handler(httpd_req_t *req);
static esp_err_t list_handler(httpd_req_t *req);
static esp_err_t download_handler(httpd_req_t *req);

// ===================
// 初始化
// ===================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  
  // 初始化板载LED
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
  
  Serial.println("=== ESP32-CAM 定时拍照装置 ===");
  
  // 初始化摄像头
  if (!initCamera()) {
    Serial.println("摄像头初始化失败!");
    delay(1000);
    ESP.restart();
  }
  
  // 初始化SD卡
  sdCardAvailable = initSDCard();
  
  // 连接WiFi
  if (ENABLE_WEB_SERVER) {
    Serial.println("正在连接WiFi...");
    WiFi.begin(ssid, password);
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      Serial.print(".");
      retry++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi已连接!");
      Serial.print("IP地址: ");
      Serial.println(WiFi.localIP());
      
      // 启动Web服务器
      startWebServer();
    } else {
      Serial.println("\nWiFi连接失败，继续离线模式");
    }
  }
  
  Serial.println("\n系统就绪!");
  Serial.printf("拍照间隔: %lu 秒\n", CAPTURE_INTERVAL / 1000);
  Serial.println("============================");
}

// ===================
// 主循环
// ===================
void loop() {
  unsigned long currentTime = millis();
  
  // 检查是否到达拍照时间
  if (currentTime - lastCaptureTime >= CAPTURE_INTERVAL) {
    lastCaptureTime = currentTime;
    
    Serial.println("\n--- 定时拍照触发 ---");
    
    if (captureAndSavePhoto()) {
      photoCount++;
      Serial.printf("已拍摄 %d 张照片\n", photoCount);
    }
    
    Serial.println("-------------------");
  }
  
  delay(100);  // 短暂延时，降低CPU占用
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

  // 初始化摄像头
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败，错误码: 0x%x\n", err);
    return false;
  }

  // 获取摄像头传感器
  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    // 调整图像参数
    s->set_brightness(s, 0);      // 亮度 -2 到 2
    s->set_contrast(s, 0);        // 对比度 -2 到 2
    s->set_saturation(s, 0);      // 饱和度 -2 到 2
    s->set_special_effect(s, 0);  // 特效 0-6
    s->set_whitebal(s, 1);        // 白平衡
    s->set_awb_gain(s, 1);        // AWB增益
    s->set_wb_mode(s, 0);         // WB模式
    s->set_exposure_ctrl(s, 1);   // 曝光控制
    s->set_aec2(s, 0);            // AEC2
    s->set_gain_ctrl(s, 1);       // 增益控制
    s->set_agc_gain(s, 0);        // AGC增益
    s->set_gainceiling(s, (gainceiling_t)0);  // 增益上限
    s->set_bpc(s, 0);             // 黑像素校正
    s->set_wpc(s, 1);             // 白像素校正
    s->set_raw_gma(s, 1);         // 原始伽马
    s->set_lenc(s, 1);            // 镜头校正
    s->set_hmirror(s, 0);         // 水平镜像
    s->set_vflip(s, 0);           // 垂直翻转
    s->set_dcw(s, 1);             // 下采样
    s->set_colorbar(s, 0);        // 彩条测试
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

  Serial.print("SD卡类型: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("未知");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD卡容量: %llu MB\n", cardSize);

  // 创建照片目录
  if (!SD_MMC.exists("/photos")) {
    SD_MMC.mkdir("/photos");
    Serial.println("创建 /photos 目录");
  }

  Serial.println("SD卡初始化成功");
  return true;
}

// ===================
// 获取时间戳字符串
// ===================
String getTimestamp() {
  // 获取当前时间（从启动开始的毫秒数）
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
  // LED闪烁表示正在拍照
  digitalWrite(LED_GPIO_NUM, HIGH);
  
  // 捕获图像
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("拍照失败: 无法获取帧缓冲区");
    digitalWrite(LED_GPIO_NUM, LOW);
    return false;
  }

  String timestamp = getTimestamp();
  String path = "/photos/photo_" + timestamp + ".jpg";

  if (sdCardAvailable) {
    // 保存到SD卡
    fs::FS &fs = SD_MMC;
    File file = fs.open(path.c_str(), FILE_WRITE);
    if (!file) {
      Serial.println("无法创建文件: " + path);
      esp_camera_fb_return(fb);
      digitalWrite(LED_GPIO_NUM, LOW);
      return false;
    }

    file.write(fb->buf, fb->len);
    file.close();
    
    Serial.printf("照片已保存: %s (%d bytes)\n", path.c_str(), fb->len);
  } else {
    Serial.printf("拍照完成 (SD卡不可用): %d bytes\n", fb->len);
  }

  // 释放帧缓冲区
  esp_camera_fb_return(fb);
  
  // 关闭LED
  digitalWrite(LED_GPIO_NUM, LOW);
  
  return true;
}

// ===================
// Web服务器处理函数
// ===================

// 主页HTML
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM 定时拍照</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        h1 { color: #333; text-align: center; }
        .btn { display: inline-block; padding: 10px 20px; margin: 5px; background: #007bff; color: white; text-decoration: none; border-radius: 5px; }
        .btn:hover { background: #0056b3; }
        .btn-green { background: #28a745; }
        .btn-green:hover { background: #1e7e34; }
        .info { background: #e9ecef; padding: 15px; border-radius: 5px; margin: 10px 0; }
        .photo-list { max-height: 400px; overflow-y: auto; border: 1px solid #ddd; padding: 10px; }
        .photo-item { padding: 5px; border-bottom: 1px solid #eee; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📷 ESP32-CAM 定时拍照装置</h1>
        <div class="info">
            <p><strong>状态:</strong> 运行中</p>
            <p><strong>拍照间隔:</strong> %INTERVAL% 秒</p>
            <p><strong>已拍摄照片数:</strong> %COUNT%</p>
        </div>
        <div style="text-align: center;">
            <a href="/capture" class="btn btn-green">立即拍照</a>
            <a href="/list" class="btn">查看照片列表</a>
        </div>
    </div>
</body>
</html>
)rawliteral";

// 主页处理
static esp_err_t index_handler(httpd_req_t *req) {
  String html = String(INDEX_HTML);
  html.replace("%INTERVAL%", String(CAPTURE_INTERVAL / 1000));
  html.replace("%COUNT%", String(photoCount));
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

// 立即拍照处理
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

// 照片列表处理
static esp_err_t list_handler(httpd_req_t *req) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>照片列表</title>";
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += ".photo-item{margin:10px 0;padding:10px;background:#f5f5f5;border-radius:5px;}";
  html += "a{color:#007bff;text-decoration:none;}";
  html += "a:hover{text-decoration:underline;}</style></head><body>";
  html += "<h1>📁 照片列表</h1><a href='/'>← 返回首页</a><hr>";
  
  if (sdCardAvailable) {
    File root = SD_MMC.open("/photos");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      int count = 0;
      while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".jpg")) {
          html += "<div class='photo-item'>";
          html += "📷 " + String(file.name());
          html += " (" + String(file.size() / 1024) + " KB) ";
          html += "<a href='/download?file=" + String(file.name()) + "'>下载</a>";
          html += "</div>";
          count++;
        }
        file = root.openNextFile();
      }
      if (count == 0) {
        html += "<p>暂无照片</p>";
      }
    }
  } else {
    html += "<p>SD卡不可用</p>";
  }
  
  html += "</body></html>";
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

// 下载处理
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
          httpd_resp_set_hdr(req, "Content-Disposition", 
                            ("attachment; filename=\"" + String(filename) + "\"").c_str());
          
          size_t chunk_size = 512;
          uint8_t buffer[512];
          while (file.available()) {
            size_t read_len = file.read(buffer, chunk_size);
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

// 启动Web服务器
void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t list_uri = {
    .uri       = "/list",
    .method    = HTTP_GET,
    .handler   = list_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t download_uri = {
    .uri       = "/download",
    .method    = HTTP_GET,
    .handler   = download_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &list_uri);
    httpd_register_uri_handler(camera_httpd, &download_uri);
    Serial.println("Web服务器已启动 (端口 80)");
  }
}
