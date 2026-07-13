IoT Smart Door Monitor

An ESP32-based motion alert system built to notify shop staff the moment a customer enters, without anyone needing to be stationed at the door. Motion is detected by a PIR sensor and pushed out as an instant, muteable Telegram alert to multiple phones at once, with every event also logged to a Google Sheet for a simple activity record.

Solo project — hardware, enclosure, firmware, and both API integrations designed and built end-to-end by me.

Features


Real-time motion detection using a PIR sensor wired to an ESP32
Instant Telegram alerts sent to multiple phones simultaneously when motion is detected
Per-user mute/unmute via inline Telegram keyboard buttons — each recipient controls their own alerts independently, no app required
Automatic activity logging to Google Sheets via a Google Apps Script Web App, with timestamps pulled from a public time API for consistency
Custom 3D-printed enclosure designed to house the ESP32, PIR sensor, and antenna discreetly at the door


How it works


The ESP32 connects to WiFi on boot and sends a "system online" message to each configured Telegram chat, along with On/Off buttons for controlling alerts.
When the PIR sensor detects motion (with a debounce interval to avoid duplicate triggers), the device:

Fetches a timestamp from a public time API
Logs the event to a Google Sheet via a Google Apps Script Web App endpoint
Sends a Telegram message to every user who currently has alerts enabled



A separate polling loop checks Telegram for incoming button presses, letting each user toggle their own notifications on or off at any time.


Hardware


ESP32 development board
PIR motion sensor
Antenna
Custom 3D-printed enclosure


Setup

This repo ships with placeholder values in place of real credentials. To run it yourself:


Open ESP_Alone_v2_0.ino in the Arduino IDE (with ESP32 board support installed)
Replace the following placeholders with your own values:

YOUR_WIFI_SSID / YOUR_WIFI_PASSWORD — your WiFi network credentials
YOUR_TELEGRAM_BOT_TOKEN — create a bot via @BotFather on Telegram to get one
YOUR_CHAT_ID_1/2/3 — the Telegram chat IDs of whoever should receive alerts (message @userinfobot to find your own)
YOUR_GOOGLE_APPS_SCRIPT_WEB_APP_URL — deploy your own Google Apps Script as a Web App to log to a Google Sheet, and paste its URL here



Wire the PIR sensor's output pin to GPIO 13 (or update pirPin in the code to match your wiring)
Flash to the ESP32


Known issues / next steps


The device currently assumes a constant power supply. An unstable connection can cause unexpected restarts, which may lead to repeated or duplicate alerts.
Planned fix: add a software debounce/state-check on boot, and/or a hardware fix to ensure stable power delivery.


Author

Naoise Lowry — Electronic and Computer Engineering student, University of Galway
