#include <WiFiClientSecure.h>    // enables secure (HTTPS / TLS) connection over WiFi
#include <PubSubClient.h>        // MQTT client library (used for AWS IoT communication)
#include <ArduinoJson.h>         // JSON parsing and creation (API + MQTT payload handling)
#include <HTTPClient.h>          // HTTP/HTTPS requests (used to call REST APIs)
#include <SPI.h>                 // SPI communication (used by display or other peripherals)
#include <TFT_eSPI.h>            // TFT display driver library (for screen UI)
#include <set>                   // STL container to store unique values (used for processed payment IDs)
#include <WiFi.h>                // WiFi connection handling for ESP32
#include"global.h"               // your custom global variables and flags
#include "secrets.h"             // stores WiFi credentials, AWS endpoint, certificates (IMPORTANT: keep private)
#include "NotoSansBold15.h"      // custom font (medium size)
#include "NotoSansBold36.h"      // custom font (large size)
#include "qrcode_espi.h"         // include QR code generation library for TFT display
#include "esp_task_wdt.h"
// ---------------- GPIO PIN DEFINITIONS ----------------
#define START_PIN  26    // Input: Trigger to start order / dispensing    
#define READY_PIN  25    // Output: Indicates system is ready  
#define DISPENSE_PIN 27  // Output: Controls dispensing mechanism (relay/motor)
#define INPUT_PIN  34   // Input-only pin (sensor or trigger input) 
#define OUTPUT_PIN 33   // General-purpose output
#define OUTPUT_PIN_1 14  // Additional output control

// ---------------- FONT CONFIGURATION ----------------
#define AA_FONT_SMALL NotoSansBold15  // Font used for small UI text
#define AA_FONT_LARGE NotoSansBold36  // Font used for headings

// ---------------- COLOR DEFINITIONS ----------------
#define TFT_LIGHTGRAY 0xC618
#define TFT_NAVY 0x000F 
#define TFT_PINK 0xF81F  // Pink background (used for QR screen)
#define TFT_DARKGREEN   0x03E0  // Green (used for success screen)
#define TFT_RED 0xF800  // Red (used for failure screen)
#define TFT_CYAN 0x07FF // Cyan (used for loading screen)
#define TFT_BL 32 //backlight for the display

// API to create a payment order (returns QR/payment link)
#define CREATE_ORDER_API  "https://3xrml0xznk.execute-api.eu-north-1.amazonaws.com/production/createOrder"
// API to fetch dynamic UI text + amount (from backend)
#define GET_AMOUNT_API    "https://xd2fqa1j3c.execute-api.eu-north-1.amazonaws.com/Production/GetAmountDetails"
// API to update dispensing/toilet status (busy/idle)
#define UPDATE_STATUS_API "https://pen0xi2jmc.execute-api.eu-north-1.amazonaws.com/Production/updateStatus"

// ---------------- MACHINE IDENTIFICATION ----------------
String default_machine_id = "vending_machine_02";  // Unique ID for this device (used in APIs & MQTT topics)
String toiletDetailsId = "vending_machine_02";   // ID used specifically for backend toilet/payment APIs
WiFiClientSecure net;             // Secure client for TLS communication (used by MQTT & HTTPS)
PubSubClient client(net);        // MQTT client instance using secure connection                   
std::set<String> processedPaymentIDs;   // Stores already processed payment IDs to prevent duplicate processing (important for reliability)
String displayHeading;    
String subHeading1;
String subHeading2;
String amount;
// NEW SAFE CONTROL (DO NOT REMOVE OLD VARIABLES YET)
enum SystemState {
  IDLE,
  ORDER,
  PROCESSING,
  WAIT_PAYMENT,
  PAID,
  DISPENSE,
  TIMEOUT,
  ERROR
};

SystemState state = IDLE;
unsigned long lastApiCall = 0;
unsigned long stateTimer = 0;
unsigned long successScreenStart = 0;
unsigned long timeoutScreenStart = 0;
unsigned long dispenseStart = 0;
unsigned long heapTimer = 0;
unsigned long reconnectTimer = 0;
const unsigned long reconnectInterval = 10000;
// Your API URL
/*bool PersonDetected = false;
bool orderInProgress = false;
bool initialMessageDisplayed = false;
bool qrCodeVisible = false;
bool paymentCompleted = false;*/
int previousStatus = -1; // Tracks last status sent to backend (avoids redundant API calls),Example: 0 = idle, 1 = dispensing
unsigned long qrStartTime = 0;  // Store the start time of QR code display
unsigned long lastDisplayedTime = 0;  // Tracks last timer value shown (avoids unnecessary redraws)
TFT_eSPI display = TFT_eSPI();
QRcode_eSPI qrcode(&display);
bool mqttFlag = false;
String mqttData = "";

// AWS IoT MQTT Topics
String AWS_IOT_PUBLISH_TOPIC = "vending/machine/" + default_machine_id + "/payment/request";  // Topic used to publish payment requests (if needed)
String AWS_IOT_SUBSCRIBE_TOPIC = "vending/machine/" + default_machine_id + "/payment/status"; // Topic subscribed to receive payment status updates from AWS

String fetchAmount(String toiletDetailsId) 
{
    HTTPClient http; // HTTP client instance
// Check WiFi connection
    if (WiFi.status() == WL_CONNECTED) 
    {
        http.begin(GET_AMOUNT_API); // API endpoint
        http.setTimeout(5000);
        // Create JSON payload with ToiletDetailsID
        //String payload = "{\"ToiletDetailsID\": \"" + toiletDetailsId + "\"}";
        String payload;
        payload.reserve(100);
        payload = "{\"ToiletDetailsID\": \"" + toiletDetailsId + "\"}";
        http.addHeader("Content-Type", "application/json");   // Set the request header
       int httpCode = http.POST(payload);  // Send POST request with JSON body
// If request successful
    if (httpCode == 200) 
    {
        String response;
        response.reserve(512);
        response = http.getString();
        http.end();
        return response; // Expected to return the amount and other details as a JSON string
    } 
    else 
    {
        Serial.println("Error fetching amount: HTTP " + String(httpCode));
        http.end();
        return "ERROR";
    }
  } else
    {
        Serial.println("WiFi not connected.");
        return "ERROR";
    }
}

String updateDispensingStatus(String toiletDetailsId, int status)
 {
    // Proceed only if WiFi is connected
    if (WiFi.status() == WL_CONNECTED)
    {
        HTTPClient http;
        http.begin(UPDATE_STATUS_API);  // Initialize POST request
        http.setTimeout(5000);
 // Construct JSON payload (nested JSON string inside "body")
        String payload;
        payload.reserve(200);
        payload = "{\"body\": \"{\\\"ToiletDetailsID\\\": \\\"" + toiletDetailsId + "\\\", \\\"Status\\\": " + String(status) + "}\"}";
        http.addHeader("Content-Type", "application/json");  // Set content type
        Serial.println("Sending payload: " + payload);   // Debug payload
        int httpCode = http.POST(payload); // Send POST request
// Check if request succeeded
    if (httpCode == 200)
     {
        String response;
        response.reserve(512);
        response = http.getString();
        Serial.println("Toilet status updated: " + response);
        http.end();
        return response;
    } 
    else 
    {
        Serial.println("Error updating Toilet status: " + String(httpCode));
        http.end();
        return "ERROR";
    }
  } 
  else
   {
        Serial.println("Wi-Fi not connected. Not able to update the Toilet status");
        return "ERROR";
  }
}

String parseJsonValue(String jsonString, String key) 
{
    StaticJsonDocument<200> doc;  // JSON buffer (size depends on response)
 // Parse JSON string
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error) 
    {
    Serial.println("Failed to parse JSON");
    return "";  // Return empty if parsing fails
    }
    // Check if key exists in JSON
    if (doc.containsKey(key)) 
    {
        return doc[key].as<String>();  // Return the value as String
    } 
    else 
    {
        Serial.println("Key not found: " + key);
        return "";  // Return empty if key missing
    }
}
//FUNCTION PROTOTYPES
// ---------------- AWS / MQTT ----------------
void connectAWS();    // Establish WiFi + AWS IoT MQTT connection and subscribe to topics
void createOrder(int amount, String machine_id);   // Calls backend API to create a payment order and fetch QR/payment link
void messageHandler(char* topic, byte* payload, unsigned int length);   // MQTT callback: handles incoming payment status messages from AWS IoT
// ---------------- UI DISPLAY FUNCTIONS ----------------
void displayInitialMessage();  // Shows default idle screen (e.g., "Machine Ready / Waiting for Orders")
void displayQRCode(String paymentLink);  // Displays QR code for payment (NOTE: prototype mismatch with actual definition in your code)
void displayPaymentStatus(String status);  // Displays payment status text (e.g., "Processing", "Success", "Failed")
void drawGreenTickMark(int x, int y, int radius);  // Draws a green circular tick mark (used for success indication)
void showGreenTikUI();  // Displays full success UI after payment completion (NOTE: mismatch if parameters used in definition)
void showBusyUI();  // Displays "Machine Busy / In Use" screen during dispensing
// ---------------- TIMER & FLOW CONTROL ----------------
void updateTimerUI(unsigned long remainingTime);   // Updates countdown timer on screen (for QR/payment timeout)
void clearTimerUI();   // Clears timer display area after completion or timeout
void showTimeoutScreen();   // Displays timeout message when payment is not completed within allowed time
// ---------------- PAYMENT FLOW UI ----------------
void showPaymentRequestUI();   // Displays intermediate/loading UI when order is being created
void showPaymentFailUI(String payment_id, int amount);   // Displays failure UI with payment ID and amount details
// ---------------- API / DATA ----------------
void GetAmountDetails();  // Fetches dynamic UI content (heading, subheading, amount) from backend API

void connectAWS() 
{
   // Ensure Wi-Fi is connected before AWS connection
    if (WiFi.status() != WL_CONNECTED)
     {
        WiFi.mode(WIFI_STA);  // Set WiFi to station mode
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD); 
        Serial.println("Connecting to Wi-Fi");
        // Blocking wait until WiFi connects
        unsigned long wifiStart = millis();

      while (WiFi.status() != WL_CONNECTED)
    {
        if(millis() - wifiStart > 10000)
        {
            Serial.println("WiFi timeout");
            WiFi.disconnect(true);
            return;
        }
        delay(100);
        esp_task_wdt_reset();
    }
    }
   // Configure TLS certificates for secure AWS IoT connection
    net.setCACert(AWS_CERT_CA);   // Root CA certificate
    net.setCertificate(AWS_CERT_CRT);  // Device certificate
    net.setPrivateKey(AWS_CERT_PRIVATE);  // Private key
    client.setServer(AWS_IOT_ENDPOINT, 8883);  // Set AWS IoT endpoint and port (8883 = MQTT over TLS)
    client.setCallback(messageHandler);   // Register MQTT message callback handler
    client.setKeepAlive(60);   // Set MQTT keep-alive interval (seconds)
    // Ensure MQTT is connected
    if (!client.connected()) 
    {
        Serial.println("Connecting to AWS IoT Core");
        unsigned long mqttStart = millis();

        while (!client.connect(THINGNAME))
        {
            Serial.print(".");
            if(millis() - mqttStart > 10000)
            {
                Serial.println("\nMQTT Connection Timeout");
                return;
            }
            delay(250);
            esp_task_wdt_reset();
        }
        // Double-check connection status
        if (!client.connected())
        {
            Serial.println("AWS IoT Timeout!");
            return;
        }
        // Subscribe to payment status topic
        client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC.c_str());
        Serial.println("AWS IoT Connected!");
    }
}


void createOrder(int amount, String machine_id)
{
    HTTPClient http;

    String url = String(CREATE_ORDER_API);

    http.begin(url);

    http.setTimeout(5000);

    String esp32_address = WiFi.localIP().toString();

    StaticJsonDocument<256> doc;

    doc["amount"] = amount;
    doc["machine_id"] = machine_id;
    doc["esp32_address"] = esp32_address;

    String payload;
    payload.reserve(256);
    serializeJson(doc, payload);

    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0)
    {
        String response;
        response.reserve(512);
        response = http.getString();

        Serial.println(response);

        StaticJsonDocument<512> doc;

        DeserializationError error =
            deserializeJson(doc, response);

        if (error)
        {
            Serial.println("JSON Parse Failed");
            return;
        }

        String paymentURL = doc["qr_string"];

        Serial.println("QR DATA:");
        Serial.println(paymentURL);

        displayQRCode(paymentURL,amount,displayHeading,subHeading1,subHeading2);
        state = WAIT_PAYMENT;
        stateTimer = millis();
    }
    else
    {
        Serial.println("Error in createOrder");
    }

    http.end();
}



void messageHandler(char* topic, byte* payload, unsigned int length)
{
    mqttData = String((char*)payload, length);
    mqttFlag = true;
}


void displayInitialMessage()
{
   
    display.fillScreen(0x03E0);   // fill screen with dark green color
    display.setTextColor(TFT_WHITE, 0x03E0);  // White text on dark green background
    display.setTextDatum(CC_DATUM);  // Center alignment
    display.loadFont(AA_FONT_SMALL);   // load font
     display.drawString("Machine is Ready", display.width() / 2, display.height() / 3.2);  // display main message
    display.unloadFont();
    display.setTextColor(TFT_WHITE, 0x03E0);  // White text on dark green background
    display.setTextDatum(CC_DATUM);  // Center alignment
    display.loadFont(AA_FONT_SMALL);  // Use small font for the instruction
    display.drawString("Waiting for Orders!!!", display.width() / 2, display.height() / 2 + 15);
    display.unloadFont();
}

void displayQRCode(String paymentLink, int amount, String displayHeading, String subHeading1, String subHeading2)
{
    qrStartTime = millis();  // store QR start time
   
    display.fillScreen(TFT_PINK);  // clear screen with pink background
 // Create QR code
    qrcode.init();
   qrcode.create(paymentLink.c_str(), 15, 60, 180);
 // Display Heading
    display.setTextColor(TFT_NAVY, TFT_LIGHTGRAY);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_LARGE);
    int xpos = display.width() / 2;  // center X position
    int ypos = 12;   // top Y position
    display.drawString(displayHeading, xpos, ypos);  // Dynamic Display Heading
    display.unloadFont();
    float displayAmount = amount/100.0;  // store amount
 // Subheading 1 (Entry fee + amount)
    String subheading1WithAmount = subHeading1 + " " + String(displayAmount);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    ypos = 51;  // position
    display.drawString(subheading1WithAmount, xpos, ypos);  // Dynamic Subheading 1 with amount
    display.unloadFont();
 // Subheading 2 (Payment instructions)
    display.loadFont(AA_FONT_SMALL);
    display.setTextDatum(TC_DATUM);
    ypos = 72;
    display.drawString(subHeading2, xpos, ypos);  // Dynamic Subheading 2
    display.unloadFont();
}

void displayPaymentStatus(String status) 
{
    display.fillScreen(TFT_WHITE);    // clear screen
    display.setTextColor(TFT_NAVY);   // set text color
    display.setTextDatum(TC_DATUM);   // align top center
    display.loadFont(AA_FONT_SMALL);  // load font
    int xpos = display.width() / 2;    // center X
    int ypos = display.height() / 2;   // center Y
    display.drawString("Payment " + status, xpos, ypos);
}

void drawGreenTickMark(int x, int y, int radius) 
{
  display.fillCircle(x, y, radius, TFT_DARKGREEN);  // Fill with green color
  int tickLength = radius / 1.5;  // Adjust tick length for better visual proportion
  display.drawLine(x - tickLength / 2, y, x, y + tickLength / 2, TFT_WHITE);  // Left part of the tick
  display.drawLine(x, y + tickLength / 2, x + tickLength / 2, y - tickLength / 2, TFT_WHITE);  // Right part of the tick
}

void showGreenTikUI(String payment_id, int amount) 
{
    display.fillRect(0, 0, display.width(), display.height() / 2, TFT_DARKGREEN);    // Fill the upper half with green
     display.fillRect(0, display.height() / 2, display.width(), display.height() / 2, TFT_WHITE);  // Fill the lower half with white
    int centerX = display.width() / 2;
    int centerY = display.height() / 4;
    int radius = 35;  // Radius of the circle
    int borderThickness = 3;  // Increase the border thickness
 // Draw the circle with multiple borders to simulate thickness
    for (int i = 0; i < borderThickness; i++) 
    {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }
    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_DARKGREEN);  // Reduced radius to create space for border
 // Draw a white tick mark inside the green circle with increased thickness
    int tickThickness = 4; // Adjust this value for thicker tick marks
 // Draw the left part of the tick with multiple lines to increase thickness
    for (int i = -tickThickness / 2; i <= tickThickness / 2; i++) 
    {
        display.drawLine(centerX - radius / 3, centerY + i, centerX, centerY + radius / 2 + i, TFT_WHITE);
    }
 // Draw the right part of the tick with multiple lines to increase thickness
    for (int i = -tickThickness / 2; i <= tickThickness / 2; i++) 
    {
        display.drawLine(centerX, centerY + radius / 2 + i, centerX + radius / 3, centerY - radius / 3 + i, TFT_WHITE);
    }
 // Display "SUCCESS" text below the circle
    display.setTextColor(TFT_WHITE, TFT_DARKGREEN);  // White text on green background
    display.setTextDatum(CC_DATUM);  // Center of the text (fixed issue: use CC_DATUM)
    display.loadFont(AA_FONT_SMALL);  // Load smaller font for the label
    display.drawString("SUCCESS", centerX, centerY + radius + 18);  // Draw text 10 pixels below the circle
    display.unloadFont();  // Unload the font after usage
 // Display "Payment Received" label in larger font
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.setTextDatum(TC_DATUM);  // Top Center datum for alignment
    display.loadFont(AA_FONT_SMALL);  // Load large font for the label
    display.drawString("Payment Received", centerX, display.height() / 2 + 30);  // Position it near the center bottom
    display.unloadFont();  // Unload the font after usage
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString("Payment ID:", centerX, display.height() / 2 + 50);  // Position it just below the payment label
    display.unloadFont();
 // Display Payment ID dynamically
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString(payment_id, centerX, display.height() / 2 + 70);  // Position it just below the payment label
    display.unloadFont();  // Unload the font after usage
 // Display Amount dynamically
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
 //display.drawString("Amount: Rs " + int(amount), centerX, display.height() / 2 + 90);  // Position it just below the payment label
     String amountStr = "Amount: " + String(amount);
    display.drawString(amountStr, centerX, display.height() / 2 + 90);
    Serial.print("Amount:");
    Serial.print(amount);
    display.unloadFont();  // Unload the font after usage
    
}

void showBusyUI()
{
// Set the background to white
    display.fillScreen(TFT_WHITE);  // White background
// Define the timer icon's position and radius
    int iconX = display.width() / 2;  // Horizontal center
    int iconY = display.height() / 3;  // Position near the top part of the screen
    int iconRadius = 30;  // Reduced radius of the outer circle representing the timer
    int borderThickness =15;  // Thickness of the circle's border
// Draw the outer circle for the timer icon in black (thicker border)
    display.drawCircle(iconX, iconY, iconRadius, TFT_BLACK);  // Black circle border
    display.fillCircle(iconX, iconY, iconRadius - borderThickness, TFT_WHITE); // Fill the inside of the circle to simulate thicker border
// Define the triangle size (static, not animated)
    int triangleHeight = iconRadius - 10 - 10;  // Adjusted height for the triangles to fit inside the circle
// Top triangle: Representing the upper part of the hourglass
    display.fillTriangle(
        iconX - triangleHeight, 
        iconY - iconRadius + triangleHeight,   // Left point
        iconX + triangleHeight, 
        iconY - iconRadius + triangleHeight,   // Right point
        iconX, 
        iconY - iconRadius + iconRadius,  // Top point (center)
        TFT_BLACK);  // Black triangle to simulate upper part
 // Bottom triangle: Representing the lower part of the hourglass
    display.fillTriangle(
        iconX - triangleHeight, 
        iconY + iconRadius - triangleHeight,   // Left point
        iconX + triangleHeight, 
        iconY + iconRadius - triangleHeight,   // Right point
        iconX, 
        iconY + iconRadius - iconRadius,  // Bottom point (center)
        TFT_BLACK);  // Black triangle to simulate lower part
 // Display the "Toilet is Busy" text below the icon in black
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Black text on white background
    display.setTextDatum(CC_DATUM);  // Center alignment
    display.loadFont(AA_FONT_SMALL);  // Use a smaller font for the label
    display.drawString("Toilet in Use", display.width() / 2, iconY + iconRadius + 25);
    display.unloadFont();  // Unload font after usage
 // Display "Please wait to complete the current order" text below the first label in black
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Black text on white background
    display.setTextDatum(CC_DATUM);  // Center alignment
    display.loadFont(AA_FONT_SMALL);  // Use the same small font for the second label
    display.drawString("Please wait...", display.width() / 2, iconY + iconRadius + 50);
    display.unloadFont();  // Unload font after usage
}

void updateTimerUI(unsigned long remainingTime) 
{
 // Clear the previous timer display area
    display.fillRect(0, display.height() - 30, display.width(), 30, TFT_WHITE);
    int minutes = remainingTime / 60;   // calculate minutes
    int seconds = remainingTime % 60;   // calculate seconds
    char timerBuffer[6];
    snprintf(timerBuffer, sizeof(timerBuffer), "%02d:%02d", minutes, seconds);  // format time
 // Display the countdown timer
    display.setTextColor(TFT_RED, TFT_PINK);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Time left: " + String(timerBuffer), display.width() / 2, display.height() - 15);  
    display.unloadFont();
}

void clearTimerUI() 
{
 // Clear the timer area
    display.fillRect(0, display.height() - 30, display.width(), 30, TFT_WHITE);
}

void showTimeoutScreen()
{
 // Clear the display and show timeout message
    //qrCodeVisible = false;
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Payment Timeout", display.width() / 2, display.height() / 2 - 20);
    display.unloadFont();
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Please try again.", display.width() / 2, display.height() / 2 + 10);
    display.unloadFont();
    display.loadFont(AA_FONT_SMALL);
    display.setTextDatum(TC_DATUM);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.drawString("Retry in:",display.width()/2,display.height()-45);
    display.unloadFont();
    timeoutScreenStart = millis();
    state = TIMEOUT;
    //paymentCompleted = false;
    //orderInProgress = false;
}

void showPaymentRequestUI()
{
    display.fillRect(0, 0, display.width(), display.height() / 2, TFT_CYAN);
 // Fill the lower half with white
    display.fillRect(0, display.height() / 2, display.width(), display.height() / 2, TFT_WHITE);
 // Increase circle border thickness by drawing multiple circles
    int centerX = display.width() / 2;
    int centerY = display.height() / 4;
    int radius = 35;  // Radius of the circle
    int borderThickness = 3;  // Increase the border thickness
 // Draw the circle with multiple borders to simulate thickness
    for (int i = 0; i < borderThickness; i++)
    {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }
 // Draw a filled green circle (inside the white border)
    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_CYAN);  // Reduced radius to create space for border
 // Display "SUCCESS" text below the circle
    display.setTextColor(TFT_WHITE, TFT_DARKGREEN);  // White text on green background
    display.setTextDatum(CC_DATUM);  // Center of the text (fixed issue: use CC_DATUM)
    display.loadFont(AA_FONT_SMALL);  // Load smaller font for the label
    display.drawString("Loading", centerX, centerY + radius + 18);  // Draw text 10 pixels below the circle
    display.unloadFont();  // Unload the font after usage
 // Display "Payment Received" label in larger font
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.setTextDatum(TC_DATUM);  // Top Center datum for alignment
    display.loadFont(AA_FONT_SMALL);  // Load large font for the label
    display.drawString("Payment Received", centerX, display.height() / 2 + 30);  // Position it near the center bottom
    display.unloadFont();  // Unload the font after usage
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Dark green text on white background
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString("Payment ID:", centerX, display.height() / 2 + 50);  // Position it just below the payment label
    display.unloadFont();
}

void showPaymentFailUI(String payment_id, int amount) 
{
 // Fill the upper half with red (payment failed)
    display.fillRect(0, 0, display.width(), display.height() / 2, TFT_RED);
 // Fill the lower half with white
    display.fillRect(0, display.height() / 2, display.width(), display.height() / 2, TFT_WHITE);
 // Circle parameters
    int centerX = display.width() / 2;
    int centerY = display.height() / 4;
    int radius = 35;  // Radius of the circle
    int borderThickness = 3;  // Increase the border thickness
 // Draw the circle with multiple borders to simulate thickness
    for (int i = 0; i < borderThickness; i++) 
    {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }
    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_RED); 
    int crossThickness = 6;  // Adjust the thickness of the cross
 // Draw the diagonal lines for the cross with a thicker effect by offsetting
    for (int offset = -crossThickness / 2; offset <= crossThickness / 2; offset++) 
    {
        display.drawLine(centerX - radius / 2, centerY - radius / 2 + offset, centerX + radius / 2, centerY + radius / 2 + offset, TFT_WHITE);  // Diagonal line from top-left to bottom-right
        display.drawLine(centerX + radius / 2, centerY - radius / 2 + offset, centerX - radius / 2, centerY + radius / 2 + offset, TFT_WHITE);  // Diagonal line from top-right to bottom-left
    }
 // Display "PAYMENT FAILED" text below the circle
    display.setTextColor(TFT_WHITE, TFT_RED);  // White text on red background
    display.setTextDatum(CC_DATUM);  // Center of the text (fixed issue: use CC_DATUM)
    display.loadFont(AA_FONT_SMALL);  // Load smaller font for the label
    display.drawString("PAYMENT FAILED", centerX, centerY + radius + 18);  // Draw text 10 pixels below the circle
    display.unloadFont();  // Unload the font after usage
 // Adjust Y-coordinates to add enough space between the labels
    int startY = display.height() / 2 + 30;  // Start position for the text labels
 // Display "Payment Failed" label in black text
    display.setTextColor(TFT_BLACK, TFT_WHITE);  // Black text on white background
    display.setTextDatum(TC_DATUM);  // Top Center datum for alignment
 // Display dynamic Payment ID
    startY += 20;  // Increase Y to avoid overlap with the previous label
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString("Payment ID:", centerX, startY);  // Position it just below the payment label
    display.unloadFont();  // Unload font after usage
    startY += 20;  // Increase Y to avoid overlap with the previous label
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString(payment_id, centerX, startY);  // Position it just below the payment label
    display.unloadFont();  // Unload font after usage
 // Display dynamic Amount
    startY += 20;  // Increase Y to avoid overlap with the previous label
    display.loadFont(AA_FONT_SMALL);  // Load smaller font
    display.drawString("Amount: Rs " + String(amount) + "/-", centerX, startY);  // Position it just below the Payment ID
    display.unloadFont();  // Unload the font after usage
}

void GetAmountDetails(){
               String response =fetchAmount(toiletDetailsId);

                if (response != "ERROR") {
                    Serial.println("Fetched details: " + response);

                    // Parse the response to extract required values
                    displayHeading = parseJsonValue(response, "displayHeading");
                    subHeading1 = parseJsonValue(response, "subHeading1");
                    subHeading2 = parseJsonValue(response, "subHeading2");
                    amount = parseJsonValue(response, "amount");

                    // Optional debug messages
                    Serial.println("DisplayHeading: " + displayHeading);
                    Serial.println("SubHeading1: " + subHeading1);
                    Serial.println("SubHeading2: " + subHeading2);
                    
                    // Parse the response to extract the amount
                    String amount = parseJsonValue(response, "amount");
                    if (amount != "") {
                        Serial.println("Fetched amount: " + amount);

                        // Optional: Perform additional actions with the fetched details
                        // For example, passing the amount to `createOrder`
                      //  createOrder(amount.toInt(), toiletDetailsId);
                    } else {
                        Serial.println("Failed to parse amount from response.");
                    }
                } else {
                    Serial.println("Failed to fetch details.");
                }
}
void safeStopDispense()
{
    digitalWrite(DISPENSE_PIN, LOW);
    digitalWrite(READY_PIN, LOW);
}
void setup()
 {
    HTTPClient http;      // create HTTP client object (not used globally here)
    Serial.begin(115200); // start serial communication for debugging
    pinMode(TFT_BL, OUTPUT);  // set TFT backlight pin as output
    digitalWrite(TFT_BL,HIGH);  // turn ON display backlight
    connectAWS();     // connect to Wi-Fi + AWS MQTT
   // pinMode(Relay2, OUTPUT);
   // digitalWrite(Relay2, LOW);
    display.init();          // initialize TFT display
    display.setRotation(0);  // set screen orientation
    displayInitialMessage();  // show default "ready" screen
    pinMode(START_PIN, INPUT_PULLDOWN); // button input (user start)
    pinMode(READY_PIN, OUTPUT);  // ready indicator output
    pinMode(DISPENSE_PIN, OUTPUT);  // Optional: Configure dispense control
    pinMode(INPUT_PIN, INPUT);  // additional input pin
    pinMode(OUTPUT_PIN, OUTPUT);  // additional output pin
    pinMode(OUTPUT_PIN_1, OUTPUT);  // additional output pin
    digitalWrite(READY_PIN, LOW);  // Ensure READY is LOW initially
    digitalWrite(DISPENSE_PIN, LOW);  // Optional: Ensure dispensing is OFF
    updateDispensingStatus(default_machine_id, 0);   // notify server: machine is idle
    GetAmountDetails();      // fetch initial amount + UI details from API 
    esp_task_wdt_config_t wdt_config = {.timeout_ms = 30000,.idle_core_mask = (1 << portNUM_PROCESSORS) - 1,.trigger_panic = true};
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);
} 

void loop() 
{
    esp_task_wdt_reset();
    if(state == PAID)
    {
        if(millis() - successScreenStart > 2000)
        {
            displayInitialMessage();
            state = IDLE;
        }
    }

    if(state == TIMEOUT)
    {
        static int lastRemaining = -1;

        int remaining = 30 - ((millis() - timeoutScreenStart) / 1000);
        if(remaining != lastRemaining)
        {
        lastRemaining = remaining;
        char buffer[25];
        sprintf(buffer, "%02d s", remaining);
         // clear ONLY number area
        display.fillRect(display.width()/2 - 50,display.height()-30,100,30,TFT_BLACK);
        display.setTextDatum(TC_DATUM);
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.setTextSize(2);
        display.drawString(buffer,display.width()/2,display.height()-15);
        }
        if(millis() - timeoutScreenStart >= 30000)
        {
            lastRemaining = -1;
            displayInitialMessage();
            state = IDLE;
        }

    }
    // SAFE STATE TIMER HANDLER
    if (state == WAIT_PAYMENT) 
    {
        if (millis() - stateTimer > 60000) 
        {
            showTimeoutScreen();
            state = TIMEOUT;
        }
    }
    if (state == ERROR && millis() - stateTimer > 10000)
    {
        state = IDLE;
        displayInitialMessage();
    }
    if (state == TIMEOUT || state == ERROR)
    {
        safeStopDispense();
    }
    if (mqttFlag)
    {
        mqttFlag = false;
        StaticJsonDocument<200> doc;
        DeserializationError error =
        deserializeJson(doc, mqttData);
        if (!error)
        {
            if(doc.containsKey("payment_status") && doc.containsKey("payment_id") && doc.containsKey("amount"))
            {
                String status = doc["payment_status"].as<String>();
                String payment_id = doc["payment_id"].as<String>();
                int amount = doc["amount"];
                Serial.println("MQTT message received");
                if(status == "captured")
                {
                    if(processedPaymentIDs.count(payment_id))
                    {
                        Serial.println("Duplicate payment ignored");
                        return;
                    }
                    processedPaymentIDs.insert(payment_id);
                    showGreenTikUI(payment_id, amount);
                    successScreenStart = millis();
                    dispenseStart = millis();
                    state = DISPENSE; 
                }
            else
            {
                showPaymentFailUI(payment_id, amount);

                state = ERROR;

                stateTimer = millis();
            }
        }
        else
        {
            Serial.println("Invalid MQTT payload");
        }
    }
    else
    {
        Serial.println("MQTT JSON parse error");
    }
}

 #define DISPENSE_DURATION 3000

if(state == DISPENSE)
{
    digitalWrite(DISPENSE_PIN, HIGH);  // start motor/relay (IMPORTANT)

    if(millis() - dispenseStart > DISPENSE_DURATION)
    {
        Serial.println("Dispense timeout");

        safeStopDispense();   // stop motor

        state = PAID;
        
        successScreenStart = millis();

        updateDispensingStatus(default_machine_id, 0);
    }
}


    client.loop();   // keep MQTT client alive (VERY IMPORTANT)
    // check Wi-Fi connection
   if(millis() - reconnectTimer > reconnectInterval)
    {
        reconnectTimer = millis();

        if(WiFi.status() != WL_CONNECTED)
        {
            Serial.println("Reconnecting WiFi...");
            connectAWS();
        }
        else if(!client.connected())
        {
            Serial.println("Reconnecting MQTT...");
            connectAWS();
        }
    }
    

    // GLOBAL FAILSAFE 
// ===============================
    if (state != IDLE && millis() - stateTimer > 120000)
    {
        Serial.println("Failsafe reset");
        safeStopDispense();
        displayInitialMessage();
        state = IDLE;
    }
    // Check if QR code is currently displayed
    if(state == WAIT_PAYMENT)
{
    unsigned long elapsedTime = (millis() - qrStartTime) / 1000;
    unsigned long remainingTime = 60 - elapsedTime;
    if(remainingTime > 0)
    {
        if(remainingTime != lastDisplayedTime)
        {
            updateTimerUI(remainingTime);

            lastDisplayedTime = remainingTime;
        }
    }
    else
    {
        showTimeoutScreen();
        state = TIMEOUT;
        stateTimer = millis();
    }
}
  
//BUTTON DEBOUNCE 

static bool lastState = LOW;
static unsigned long lastDebounceTime = 0;

bool currentState = digitalRead(START_PIN);

if (currentState == HIGH && lastState == LOW)
{
    if (millis() - lastDebounceTime > 200)
    {
        lastDebounceTime = millis();

        if (state == IDLE &&
            WiFi.status() == WL_CONNECTED &&
            client.connected())
        {
            Serial.println("Creating Order");

            state = PROCESSING;   //  prevent double trigger
            stateTimer = millis();

            createOrder(amount.toInt(), toiletDetailsId);
        }
        else
        {
            Serial.println("Ignored button (busy/offline)");
        }
    }
}
lastState = currentState;
//HEAP MONITOR + AUTO RECOVERY
    if (millis() - heapTimer > 30000)
    {
        heapTimer = millis();
        uint32_t freeHeap = ESP.getFreeHeap();
        Serial.print("Free Heap: ");
        Serial.println(freeHeap);
    if (freeHeap < 20000)
    {
        Serial.println("Low memory → Restarting");
        ESP.restart();
    }
}

    // -------- SERIAL COMMAND HANDLING --------
    // allows manual testing/debugging via serial monitor
    if (Serial.available() > 0)
    {
        String command;
        command.reserve(100);
        command = Serial.readStringUntil('\n');
        int separator1 = command.indexOf(',');   // find first comma
        int separator2 = command.indexOf(',', separator1 + 1);      // find second comma
         // -------- CASE 1: GetAmountDetails,toiletDetailsId --------
        if (separator1 > 0 && separator2 == -1) 
        { // Handling "GetAmountDetails"
            String cmd = command.substring(0, separator1);   // command name
            String toiletDetailsId = command.substring(separator1 + 1);   // ID
            if (cmd.equalsIgnoreCase("GetAmountDetails"))
            {
                // Fetch details using API
                String response = fetchAmount(toiletDetailsId);
                if (response != "ERROR")
                {
                    Serial.println("Fetched details: " + response);
                    // extract values from JSON response
                    displayHeading = parseJsonValue(response, "displayHeading");
                    subHeading1 = parseJsonValue(response, "subHeading1");
                    subHeading2 = parseJsonValue(response, "subHeading2");
                    amount = parseJsonValue(response, "amount");
                   // debug print
                    Serial.println("DisplayHeading: " + displayHeading);
                    Serial.println("SubHeading1: " + subHeading1);
                    Serial.println("SubHeading2: " + subHeading2);
                    // extract amount again (note: case-sensitive issue possible here)
                    String amount = parseJsonValue(response, "Amount");
                    if (amount != "")
                    {
                        Serial.println("Fetched amount: " + amount);
                        // Optional: Perform additional actions with the fetched details
                        // For example, passing the amount to `createOrder`
                        if (millis() - lastApiCall > 3000)
                        {
                            lastApiCall = millis();
                            createOrder(amount.toInt(), toiletDetailsId);
                        }
                    }
                    else
                    {
                        Serial.println("Failed to parse amount from response.");
                    }
                }else
                {
                    Serial.println("Failed to fetch details.");
                }
            } 
            else
            {
                Serial.println("Unknown command.");
            }
        } 
        // -------- CASE 2: create order,machine_id,amount --------
        else if (separator1 > 0 && separator2 > separator1)
        { // Handling "create order"
            String cmd = command.substring(0, separator1);   // command
            String machine_id = command.substring(separator1 + 1, separator2);   // machine ID
            int amount = command.substring(separator2 + 1).toInt();   // amount

            if (cmd.equalsIgnoreCase("create order"))
            {
                showPaymentRequestUI();          // show loading/payment UI
                createOrder(amount, machine_id);  // create order
            }
        }
        // -------- INVALID INPUT --------
        else
        {
            Serial.println("Invalid input format. Use:");
            Serial.println("1. GetAmountDetails,toiletDetailsId");
            Serial.println("2. create order,machine_id,amount");
        }
    }
}



