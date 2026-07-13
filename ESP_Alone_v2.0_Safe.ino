#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// WiFi credentials
// TODO: Fill in your own network credentials, or better, load these from
// a separate "secrets.h" file that is excluded via .gitignore.
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Google Sheets Web App URL
// Create your own Google Apps Script Web App and paste its deployment URL here.
const char* scriptUrl = "YOUR_GOOGLE_APPS_SCRIPT_WEB_APP_URL";

// Telegram config
// Create a bot via @BotFather on Telegram to get your own token.
const char* telegramBotToken = "YOUR_TELEGRAM_BOT_TOKEN";
const String telegramApiHost = "https://api.telegram.org";

// Telegram chat IDs (hardcoded)
// Replace with the chat IDs of the phones/accounts that should receive alerts.
const String chatIDs[3] = {
  "YOUR_CHAT_ID_1",   // Primary
  "YOUR_CHAT_ID_2",   // Secondary
  "YOUR_CHAT_ID_3"    // Tertiary
};

bool alertsEnabled[3] = { true, true, true };
bool userConnected[3] = { true, true, true };
long lastUpdateId = 0;

// PIR sensor config
const int pirPin = 13;
unsigned long lastDetectionTime = 0;
const unsigned long detectionInterval = 10000; // 10 seconds

void setup() {
  Serial.begin(115200);
  pinMode(pirPin, INPUT);

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  delay(30000); // Allow PIR sensor to stabilize
  Serial.println("Sensor ready");

  for (int i = 0; i < 3; i++) {
    if (userConnected[i]) {
      sendTelegramMessageWithButtons(chatIDs[i], "🤖 System online.\nUse buttons below to control alerts.");
    }
  }
}

void loop() {
  checkTelegramCommands();

  if (digitalRead(pirPin) == HIGH && millis() - lastDetectionTime > detectionInterval) {
    Serial.println("Motion detected!");

    String timestamp = getTimestamp();
    sendToGoogleSheets(timestamp, "Motion Detected");

    for (int i = 0; i < 3; i++) {
      if (alertsEnabled[i] && userConnected[i]) {
        sendTelegramMessage(chatIDs[i], "🚪 Motion detected at door");
      }
    }

    lastDetectionTime = millis();
  }

  delay(100);
}

String getTimestamp() {
  HTTPClient http;
  http.begin("http://worldtimeapi.org/api/timezone/Etc/UTC");
  int httpCode = http.GET();
  String time = "Unavailable";

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int start = payload.indexOf("\"datetime\":\"") + 12;
    int end = payload.indexOf("\"", start);
    if (start > 11 && end > start) {
      time = payload.substring(start, end);
    }
  }
  http.end();
  return time;
}

void sendToGoogleSheets(String timestamp, String status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected!");
    return;
  }

  HTTPClient http;
  http.begin(scriptUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "timestamp=" + timestamp + "&status=" + status;
  int httpCode = http.POST(postData);

  if (httpCode == 200) {
    Serial.println("POST success!");
  } else {
    Serial.printf("POST failed with code: %d\n", httpCode);
  }
  http.end();
}

void sendTelegramMessage(String chatID, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = telegramApiHost + "/bot" + telegramBotToken + "/sendMessage";
  https.begin(client, url);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload = "chat_id=" + chatID + "&text=" + message;
  https.POST(payload);
  https.end();
}

void sendTelegramMessageWithButtons(String chatID, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = telegramApiHost + "/bot" + telegramBotToken + "/sendMessage";
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"chat_id\":\"" + chatID + "\",";
  payload += "\"text\":\"" + message + "\",";
  payload += "\"reply_markup\":{";
  payload += "\"keyboard\":[[{\"text\":\"On\"}, {\"text\":\"Off\"}]],";
  payload += "\"resize_keyboard\":true,";
  payload += "\"one_time_keyboard\":false";
  payload += "}}";

  https.POST(payload);
  https.end();
}

void checkTelegramCommands() {
  static unsigned long lastCheck = 0;
  const unsigned long checkInterval = 5000;
  if (millis() - lastCheck < checkInterval) return;
  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = telegramApiHost + "/bot" + telegramBotToken + "/getUpdates?offset=" + String(lastUpdateId + 1);
  https.begin(client, url);
  int httpCode = https.GET();

  if (httpCode == 200) {
    String response = https.getString();
    int i = 0;

    while ((i = response.indexOf("\"update_id\":", i)) != -1) {
      int idStart = i + 12;
      int idEnd = response.indexOf(",", idStart);
      long updateId = response.substring(idStart, idEnd).toInt();
      lastUpdateId = updateId;

      int textIndex = response.indexOf("\"text\":\"", idEnd);
      if (textIndex == -1) break;
      int textEnd = response.indexOf("\"", textIndex + 8);
      String messageText = response.substring(textIndex + 8, textEnd);

      int chatIdStart = response.lastIndexOf("\"id\":", textIndex);
      int chatIdEnd = response.indexOf(",", chatIdStart);
      String senderChatID = response.substring(chatIdStart + 5, chatIdEnd);

      for (int j = 0; j < 3; j++) {
        if (chatIDs[j] == senderChatID && userConnected[j]) {
          if (messageText == "Off" && alertsEnabled[j]) {
            alertsEnabled[j] = false;
            sendTelegramMessageWithButtons(chatIDs[j], "❌ Motion alerts turned *OFF*.\nTap On to enable.");
          } else if (messageText == "On" && !alertsEnabled[j]) {
            alertsEnabled[j] = true;
            sendTelegramMessageWithButtons(chatIDs[j], "✅ Motion alerts turned *ON*.\nTap Off to disable.");
          }
        }
      }

      i = textEnd;
    }
  }

  https.end();
}
