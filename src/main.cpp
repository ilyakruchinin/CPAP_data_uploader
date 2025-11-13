#include <Arduino.h>
#include <SPIFFS.h>

// Include SdWiFiBrowser library headers
#include <serial.h>
#include <config.h>
#include <network.h>
#include <sdControl.h>
#include <FSWebServer.h>

void setup() {
  SERIAL_INIT(115200);
  
  Serial.println("\n\nSD WIFI PRO Starting...");
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  Serial.println("SPIFFS mounted successfully");
  
  // Initialize SD Control
  sdcontrol.setup();
  
  // Load configuration
  config.loadFS();
  
  // Start network (will create AP or connect to WiFi)
  network.start();
  
  // Initialize web server
  server.begin(&SPIFFS);
  
  Serial.println("Setup complete!");
  Serial.println("Access via: http://192.168.4.1 (AP mode)");
}

void loop() {
  // Handle network events
  network.loop();
  
  delay(10);
}
