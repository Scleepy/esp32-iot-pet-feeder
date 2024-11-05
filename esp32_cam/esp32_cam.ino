#include "esp_camera.h"
#include <WiFi.h>
#include <esp_http_server.h>

#include <FirebaseClient.h>
#include <WiFiClientSecure.h>

const char* ssid = "SSID";
const char* password = "PASSWORD";

#define DATABASE_URL "xxx"
#define LOCAL_IP_COMMAND_PATH "/streamData/localIp"
#define IS_INITIALIZED_COMMAND_PATH "/streamData/isInitialized"

httpd_handle_t server = NULL;

WiFiClientSecure ssl;
DefaultNetwork network;
AsyncClientClass client(ssl, getNetwork(network));

FirebaseApp app;
RealtimeDatabase Database;
AsyncResult result;
NoAuth noAuth;

const int ledPin = 33;

void startCameraServer();

void initializeFirebase(){
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);

  ssl.setInsecure();
  initializeApp(client, app, getAuth(noAuth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  client.setAsyncResult(result);
}

void setStreamData(const char* localIp) {
  Serial.print("Updating Firebase with local IP: ");
  Serial.println(localIp);

  for (int i = 0; i < 5; i++) { blink(100); }

  if (!Database.set<String>(client, LOCAL_IP_COMMAND_PATH, localIp)) {
    Serial.println("Failed to set Firebase local IP.");
    return; 
  }

  Serial.println("Successfully updated Firebase with local IP.");

  for (int i = 0; i < 5; i++) { blink(50); }

  if (!Database.set<bool>(client, IS_INITIALIZED_COMMAND_PATH, false)) {
    Serial.println("Failed to set Firebase isInitialized.");
  } else {
    Serial.println("Successfully updated Firebase isInitialized to false.");
  }
}

void blink(int delayVal){
  digitalWrite(ledPin, HIGH); 
  delay(delayVal);
  digitalWrite(ledPin, LOW);
  delay(delayVal);
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  pinMode(ledPin, OUTPUT);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 15;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    blink(200);
  }
  Serial.println();
  Serial.print("Connected to WiFi! IP Address: ");
  Serial.println(WiFi.localIP());

  initializeFirebase();
  setStreamData(WiFi.localIP().toString().c_str());

  startCameraServer();
}

void loop() {
  delay(1);
}

unsigned long lastPrintTime = 0; 
int frameCount = 0; 

// Function to handle streaming frames
esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len;
  uint8_t *_jpg_buf;

  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    frameCount++;

    unsigned long currentTime = millis();
    if (currentTime - lastPrintTime >= 1000) {
      Serial.printf("FPS: %d\n", frameCount);
      lastPrintTime = currentTime;
      frameCount = 0; 
    }
    
    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb);
      if (!jpeg_converted) {
        Serial.println("JPEG compression failed");
        res = ESP_FAIL;
        break;
      }
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    char part_buf[64];
    size_t hlen = snprintf((char *)part_buf, 64, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", _jpg_buf_len);
    res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }
    if (fb->format != PIXFORMAT_JPEG) {
      free(_jpg_buf);
    }
    esp_camera_fb_return(fb);
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t stream_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &stream_uri);
  }
}
