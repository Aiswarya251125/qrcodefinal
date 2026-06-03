#include "secrets.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"
#include <HTTPClient.h>
#include "NotoSansBold15.h"
#include "NotoSansBold36.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include "qrcode_espi.h" 
#include <set>

//#define Relay2 15  // Use only Relay2 as per updated requirements
#define START_PIN  26  // Example GPIO pin for START signal
#define READY_PIN  25    // Example GPIO pin for READY signal
#define DISPENSE_PIN 27  // Optional: Pin for dispensing mechanism
// Display configuration
#define AA_FONT_SMALL NotoSansBold15
#define AA_FONT_LARGE NotoSansBold36
#define TFT_LIGHTGRAY 0xC618
#define TFT_NAVY 0x000F 
#define TFT_PINK 0xF81F 
#define TFT_DARKGREEN   0x03E0
#define TFT_RED 0xF800
#define TFT_CYAN 0x07FF

#define INPUT_PIN  34  // GPIO pin for input
#define OUTPUT_PIN 33  // GPIO pin for output
#define OUTPUT_PIN_1 14     
#define TFT_BL 32     

String default_machine_id = "vending_machine_02";
String toiletDetailsId = "vending_machine_02";
WiFiClientSecure net;
PubSubClient client(net);
std::set<String> processedPaymentIDs; 

String displayHeading;
String subHeading1;
String subHeading2;
String amount;

// AWS IoT MQTT Topics
String AWS_IOT_PUBLISH_TOPIC = "vending/machine/" + default_machine_id + "/payment/request";
String AWS_IOT_SUBSCRIBE_TOPIC = "vending/machine/" + default_machine_id + "/payment/status";

String getAmountAPI = "https://xd2fqa1j3c.execute-api.eu-north-1.amazonaws.com/Production/GetAmountDetails";
// Your API URL
bool PersonDetected = false;
bool orderInProgress = false;
bool initialMessageDisplayed = false;
int previousStatus = -1; 

unsigned long qrStartTime = 0;  // Store the start time of QR code display
bool qrCodeVisible = false;
bool paymentCompleted = false;
unsigned long lastDisplayedTime = 0; 
bool paymentReceived = false;
bool waitingForDispense = false;

String receivedPaymentId = "";
int receivedAmount = 0;

unsigned long paymentReceiveTime = 0;

TFT_eSPI display = TFT_eSPI();
QRcode_eSPI qrcode(&display);

// Function prototypes
void connectAWS();
void createOrder(int amount, String machine_id);
void messageHandler(char* topic, byte* payload, unsigned int length);
void displayInitialMessage();
void displayQRCode(String paymentLink);
void displayPaymentStatus(String status);
void drawGreenTickMark(int x, int y, int radius);
void showGreenTikUI();  // Green tick UI
void showPaymentFailUI();  // Payment fail UI
/*
void connectAWS() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); 
    Serial.println("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);
    client.setServer(AWS_IOT_ENDPOINT, 8883);
    client.setCallback(messageHandler);

    Serial.println("Connecting to AWS IoT Core");
    while (!client.connect(THINGNAME)) {
        Serial.print(".");
        delay(100);
    }
    if (!client.connected()) {
        Serial.println("AWS IoT Timeout!");
        return;
    }
    client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC.c_str());
    Serial.println("AWS IoT Connected!");
}
*/
void connectAWS() {
    // Ensure Wi-Fi is connected
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD); 
        Serial.println("Connecting to Wi-Fi");
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
    }

    // Configure AWS IoT connection certificates and endpoint
    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);
    client.setServer(AWS_IOT_ENDPOINT, 8883);
    client.setCallback(messageHandler);

    // Set Keep-Alive interval (in seconds)
    client.setKeepAlive(60); // Sends a ping every 60 seconds to maintain connection

    // Ensure MQTT is connected
    if (!client.connected()) {
        Serial.println("Connecting to AWS IoT Core");
        while (!client.connect(THINGNAME)) {
            Serial.print(".");
            delay(100);
        }
        if (!client.connected()) {
            Serial.println("AWS IoT Timeout!");
            return;
        }
        bool ok = client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC.c_str());

    Serial.print("Subscribe Topic: ");
    Serial.println(AWS_IOT_SUBSCRIBE_TOPIC);

    Serial.print("Subscribe Result: ");
    Serial.println(ok);

    Serial.println("AWS IoT Connected!");
        //client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC.c_str());
        //Serial.println("AWS IoT Connected!");
    }
}


void createOrder(int amount, String machine_id) {
    if (orderInProgress) {
        Serial.println("Order already in progress. Please wait for it to complete.");
        return;
    }
    
    orderInProgress = true;
    String url = "https://3xrml0xznk.execute-api.eu-north-1.amazonaws.com/production/createOrder";
    String esp32_address = WiFi.localIP().toString();
    String payload = "{\"amount\":" + String(amount) + ",\"machine_id\":\"" + machine_id + "\",\"esp32_address\":\"" + esp32_address + "\"}";

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Response from create order:");
        Serial.println(response);

        // Parse JSON to extract the payment URL
        StaticJsonDocument<512> doc;

DeserializationError err = deserializeJson(doc, response);

if (err) {
    Serial.print("JSON Parse Error: ");
    Serial.println(err.c_str());
    orderInProgress = false;
    return;
}

String paymentURL = doc["qr_string"];
//String paymentURL = "upi://pay?ver=01&mode=19&pa=rzpaltersoftinnova218831.rzp@ypbiz&pn=ALTERSOFTINNOVATIONSINDIAPRIVATELIMITED&tr=RZPYRZlO4citHbHV8Eqrv2&cu=INR&mc=7349&qrMedium=04&tn=PaymenttoALTERSOFTINNOVATIONSINDIAPRIVATELIMITED&am=10.00";
        Serial.println("PAYMENT URL:");
        Serial.println(paymentURL);
       displayQRCode(paymentURL, int(amount), displayHeading, subHeading1, subHeading2);
    } else {
        Serial.print("Error on sending POST: ");
        Serial.println(httpResponseCode);
        orderInProgress = false;
    }
    http.end();
}

void messageHandler(char* topic, byte* payload, unsigned int length)
{
    Serial.println("===== MQTT CALLBACK RECEIVED =====");

    StaticJsonDocument<256> doc;

    DeserializationError err =
        deserializeJson(doc, payload, length);

    if (err)
    {
        Serial.println("JSON parse failed");
        return;
    }

    String payment_status = doc["payment_status"];
    String payment_id = doc["payment_id"];
    int amount = doc["amount"];

    if (processedPaymentIDs.find(payment_id)
            != processedPaymentIDs.end())
    {
        return;
    }

    if (payment_status == "captured")
    {
        receivedPaymentId = payment_id;
        receivedAmount = amount;

        paymentReceived = true;
        waitingForDispense = true;

        paymentReceiveTime = millis();

        paymentCompleted = true;
        qrCodeVisible = false;

        Serial.println("Payment marked successful");

        processedPaymentIDs.insert(payment_id);
    }
    else
    {
        showPaymentFailUI(payment_id, amount);

        orderInProgress = false;
        paymentCompleted = true;
    }
}

void displayInitialMessage() {
    paymentCompleted = false;
    qrCodeVisible = false;

  display.fillScreen(0x03E0);  // Dark green background colo
  // Display a large, welcoming text "Smart Toilet Ready"
  display.setTextColor(TFT_WHITE, 0x03E0);  // White text on dark green background
  display.setTextDatum(CC_DATUM);  // Center alignment
  display.loadFont(AA_FONT_SMALL);  // Use large font for the title
  display.drawString("Machine is Ready", display.width() / 2, display.height() / 3.2);
  display.unloadFont();

  // Optional: Small text "Press to Start" or "Ready for Use" in white color
  display.setTextColor(TFT_WHITE, 0x03E0);  // White text on dark green background
  display.setTextDatum(CC_DATUM);  // Center alignment
  display.loadFont(AA_FONT_SMALL);  // Use small font for the instruction
  display.drawString("Waiting for Orders!!!", display.width() / 2, display.height() / 2 + 15);
  display.unloadFont();
}

void drawGreenTickMark(int x, int y, int radius) {
  display.fillCircle(x, y, radius, TFT_DARKGREEN);  // Fill with green color
  int tickLength = radius / 1.5;  // Adjust tick length for better visual proportion
  display.drawLine(x - tickLength / 2, y, x, y + tickLength / 2, TFT_WHITE);  // Left part of the tick
  display.drawLine(x, y + tickLength / 2, x + tickLength / 2, y - tickLength / 2, TFT_WHITE);  // Right part of the tick
}

void showBusyUI() {
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

void showGreenTikUI(String payment_id, int amount) {
    // Fill the upper half with green
    display.fillRect(0, 0, display.width(), display.height() / 2, TFT_DARKGREEN);

    // Fill the lower half with white
    display.fillRect(0, display.height() / 2, display.width(), display.height() / 2, TFT_WHITE);

    int centerX = display.width() / 2;
    int centerY = display.height() / 4;
    int radius = 35;  // Radius of the circle
    int borderThickness = 3;  // Increase the border thickness

    // Draw the circle with multiple borders to simulate thickness
    for (int i = 0; i < borderThickness; i++) {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }

    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_DARKGREEN);  // Reduced radius to create space for border

    // Draw a white tick mark inside the green circle with increased thickness
    int tickThickness = 4; // Adjust this value for thicker tick marks

    // Draw the left part of the tick with multiple lines to increase thickness
    for (int i = -tickThickness / 2; i <= tickThickness / 2; i++) {
        display.drawLine(centerX - radius / 3, centerY + i, centerX, centerY + radius / 2 + i, TFT_WHITE);
    }

    // Draw the right part of the tick with multiple lines to increase thickness
    for (int i = -tickThickness / 2; i <= tickThickness / 2; i++) {
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
    delay(2000);
}


void displayQRCode(String paymentLink, int amount, String displayHeading, String subHeading1, String subHeading2) {
    qrStartTime = millis();  // Record the start time
    qrCodeVisible = true;    // Set the QR code visibility flag
    paymentCompleted = false; 
    
    // Initialize display (Assuming it is already initialized)
    display.fillScreen(TFT_PINK);

    // Create QR code
    qrcode.init();
    qrcode.create(paymentLink.c_str(), 15, 60, 180);

    // Display Heading
    display.setTextColor(TFT_NAVY, TFT_LIGHTGRAY);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_LARGE);
    int xpos = display.width() / 2;
    int ypos = 12;
    display.drawString(displayHeading, xpos, ypos);  // Dynamic Display Heading
    display.unloadFont();

    float dividedAmount = amount / 100.0;

    // Subheading 1 (Entry fee + amount)
    String subheading1WithAmount = subHeading1 + " " + dividedAmount;  // Concatenate subHeading1 and amount
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    ypos = 51;
    display.drawString(subheading1WithAmount, xpos, ypos);  // Dynamic Subheading 1 with amount
    display.unloadFont();

    // Subheading 2 (Payment instructions)
    display.loadFont(AA_FONT_SMALL);
    display.setTextDatum(TC_DATUM);
    ypos = 72;
    display.drawString(subHeading2, xpos, ypos);  // Dynamic Subheading 2
    display.unloadFont();
}

void updateTimerUI(unsigned long remainingTime) {
    // Clear the previous timer display area
  
    display.fillRect(0, display.height() - 30, display.width(), 30, TFT_WHITE);

    // Format the time as MM:SS
    int minutes = remainingTime / 60;
    int seconds = remainingTime % 60;
    char timerBuffer[6];
    snprintf(timerBuffer, sizeof(timerBuffer), "%02d:%02d", minutes, seconds);

    // Display the countdown timer
    display.setTextColor(TFT_RED, TFT_PINK);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Time left: " + String(timerBuffer), display.width() / 2, display.height() - 15);
    display.unloadFont();
}


void clearTimerUI() {
    // Clear the timer area
    display.fillRect(0, display.height() - 30, display.width(), 30, TFT_WHITE);
    paymentCompleted = true;  // Mark payment as completed
}
/*
void showTimeoutScreen() {
    // Clear the display and show timeout message
    qrCodeVisible = false;
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Payment Timeout", display.width() / 2, display.height() / 2 - 20);
    display.unloadFont();

    display.loadFont(AA_FONT_SMALL);
    display.drawString("Please try again.", display.width() / 2, display.height() / 2 + 10);
    display.unloadFont();
    delay(3000);
   // displayInitialMessage(); 

}
*/
void showTimeoutScreen() {
    // Clear the display and show timeout message
    qrCodeVisible = false;
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    display.drawString("Payment Timeout", display.width() / 2, display.height() / 2 - 20);
    display.unloadFont();

    display.loadFont(AA_FONT_SMALL);
    display.drawString("Please try again.", display.width() / 2, display.height() / 2 + 10);
    display.unloadFont();

    // Wait for 1 minute before resetting to the initial state
    unsigned long startTime = millis();
    while (millis() - startTime < 30000) {
        // Optional: Display a countdown timer during the timeout screen
        unsigned long remainingTime = 30 - ((millis() - startTime) / 1000);
        display.fillRect(0, display.height() - 30, display.width(), 30, TFT_BLACK);
        display.setTextColor(TFT_YELLOW);
        display.setTextDatum(TC_DATUM);
        display.loadFont(AA_FONT_SMALL);
        char timeoutBuffer[20];
        snprintf(timeoutBuffer, sizeof(timeoutBuffer), "Retry in: %02lu s", remainingTime);
        display.drawString(String(timeoutBuffer), display.width() / 2, display.height() - 15);
        display.unloadFont();
        delay(500);  // Add a small delay to avoid flickering
    }

    // Reset to the initial state after timeout
    displayInitialMessage();
    paymentCompleted = false;
    orderInProgress = false;
}

void displayPaymentStatus(String status) {
    display.fillScreen(TFT_WHITE);
    display.setTextColor(TFT_NAVY);
    display.setTextDatum(TC_DATUM);
    display.loadFont(AA_FONT_SMALL);
    int xpos = display.width() / 2;
    int ypos = display.height() / 2;
    display.drawString("Payment " + status, xpos, ypos);
    display.unloadFont();
}

void showPaymentFailUI(String payment_id, int amount) {
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
    for (int i = 0; i < borderThickness; i++) {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }

    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_RED); 
    int crossThickness = 6;  // Adjust the thickness of the cross

    // Draw the diagonal lines for the cross with a thicker effect by offsetting
    for (int offset = -crossThickness / 2; offset <= crossThickness / 2; offset++) {
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
    //display.loadFont(AA_FONT_SMALL);  // Use small font for text
    //display.drawString("Payment Failed", centerX, startY);  // Position it at the top of the white half
    //display.unloadFont();  // Unload font after usage

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

void showPaymentRequestUI(){

    display.fillRect(0, 0, display.width(), display.height() / 2, TFT_CYAN);

    // Fill the lower half with white
    display.fillRect(0, display.height() / 2, display.width(), display.height() / 2, TFT_WHITE);

    // Increase circle border thickness by drawing multiple circles
    int centerX = display.width() / 2;
    int centerY = display.height() / 4;
    int radius = 35;  // Radius of the circle
    int borderThickness = 3;  // Increase the border thickness

    // Draw the circle with multiple borders to simulate thickness
    for (int i = 0; i < borderThickness; i++) {
        display.drawCircle(centerX, centerY, radius + i, TFT_WHITE);  // Draw multiple concentric circles to simulate thicker border
    }

    // Draw a filled green circle (inside the white border)
    display.fillCircle(centerX, centerY, radius - borderThickness / 2, TFT_CYAN);  // Reduced radius to create space for border

    
    // Display "SUCCESS" text below the circle
    display.setTextColor(TFT_WHITE, TFT_DARKGREEN);  // White text on green background
    display.setTextDatum(CC_DATUM);  // Center of the text (fixed issue: use CC_DATUM)
    display.loadFont(AA_FONT_SMALL);  // Load smaller font for the label
    display.drawString("Loding", centerX, centerY + radius + 18);  // Draw text 10 pixels below the circle
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

void setup() {
    Serial.begin(115200);
   pinMode(TFT_BL, OUTPUT);
   digitalWrite(TFT_BL,HIGH);
    connectAWS();
   // pinMode(Relay2, OUTPUT);
   // digitalWrite(Relay2, LOW);

    display.init();
    display.setRotation(0);
    displayInitialMessage(); 
    pinMode(START_PIN, INPUT);
    pinMode(READY_PIN, OUTPUT);
    pinMode(DISPENSE_PIN, OUTPUT);  // Optional: Configure dispense control
    pinMode(INPUT_PIN, INPUT);
    pinMode(OUTPUT_PIN, OUTPUT);
    pinMode(OUTPUT_PIN_1, OUTPUT);
    digitalWrite(READY_PIN, LOW);  // Ensure READY is LOW initially
    digitalWrite(DISPENSE_PIN, LOW);  // Optional: Ensure dispensing is OFF
    updateDispensingStatus(default_machine_id, 0);
    GetAmountDetails();
    
}

String fetchAmount(String toiletDetailsId) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(getAmountAPI); // Use the API endpoint

    // Create JSON payload
    String payload = "{\"ToiletDetailsID\": \"" + toiletDetailsId + "\"}";
    http.addHeader("Content-Type", "application/json"); // Set the request header

    // Send POST request with JSON body
    int httpCode = http.POST(payload);

    if (httpCode == 200) { // Check if the response is successful
      String response = http.getString();
      http.end();
      return response; // Expected to return the amount and other details as a JSON string
    } else {
      Serial.println("Error fetching amount: HTTP " + String(httpCode));
      http.end();
      return "ERROR";
    }
  } else {
    Serial.println("WiFi not connected.");
    return "ERROR";
  }
}

/*
void loop() {
    client.loop();

    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        
        int separator1 = command.indexOf(',');
        int separator2 = command.indexOf(',', separator1 + 1);

        if (separator1 > 0 && separator2 > separator1) {
            String cmd = command.substring(0, separator1);
            String machine_id = command.substring(separator1 + 1, separator2);
            int amount = command.substring(separator2 + 1).toInt();

            if (cmd.equalsIgnoreCase("create order")) {
                showPaymentRequestUI();
                createOrder(amount, machine_id);
            }
        } else {
            Serial.println("Invalid input format. Use: create order,machine_id,amount");
            orderInProgress = false;
        }
    }
}
*/
void loop() {
    client.loop();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected. Reconnecting...");
        connectAWS();
    }
    if (!client.connected()) {
        Serial.println("MQTT disconnected. Reconnecting...");
        connectAWS();
    }
    
     // Check if QR code is currently displayed
    if (qrCodeVisible) {
        unsigned long elapsedTime = (millis() - qrStartTime) / 1000;  // Elapsed time in seconds
        unsigned long remainingTime = 60 - elapsedTime;  // Remaining time

        if (!paymentCompleted) {
            if (remainingTime > 0) {
              if (remainingTime != lastDisplayedTime) {
                    updateTimerUI(remainingTime);
                    lastDisplayedTime = remainingTime;  // Update last displayed time
              }
            } else {
                showTimeoutScreen();  // Show timeout screen when time expires
                clearTimerUI();
                qrCodeVisible = false;  // Hide QR code
            }
        } else {
            clearTimerUI();  // Clear timer UI after payment
            qrCodeVisible = false;  // Hide QR code
        }
    }

    if ((digitalRead(START_PIN) == HIGH) && (orderInProgress == false)) {
        Serial.println("Creating Order");
        createOrder(amount.toInt(), toiletDetailsId);
        initialMessageDisplayed = false; // Reset flag for the initial message
    }

    if (digitalRead(START_PIN) == LOW) {
      /*
        Serial.println("Machine not ready");
        Serial.print("orderInProgress:");
        Serial.println(orderInProgress);

        Serial.print("paymentCompleted:");
        Serial.println(paymentCompleted);

        Serial.print("qrCodeVisible:");
        Serial.println(qrCodeVisible);
      */

        orderInProgress = false;
        // Only display the initial message if it hasn't been displayed
        if (!initialMessageDisplayed) {
            displayInitialMessage();
            initialMessageDisplayed = true; // Set the flag
        }
    }

  
// Payment success UI
if (paymentReceived)
{
    showGreenTikUI(receivedPaymentId,
                   receivedAmount / 100);

    digitalWrite(READY_PIN, HIGH);

    paymentReceived = false;
}


// Waiting for toilet controller to start
  if (waitingForDispense)
{
    if (millis() - paymentReceiveTime > 120000)
    {
        Serial.println("Dispense timeout");

        waitingForDispense = false;

        digitalWrite(READY_PIN, LOW);

        displayInitialMessage();
    }

    else if (digitalRead(START_PIN) == HIGH)
    {
        Serial.println("Dispensing started");

        showBusyUI();

        digitalWrite(DISPENSE_PIN, HIGH);

        updateDispensingStatus(
            toiletDetailsId,
            1
        );

        waitingForDispense = false;
    }
}

static bool dispensingActive = false;

if (digitalRead(START_PIN) == HIGH &&
    !dispensingActive)
{
    dispensingActive = true;
}

if (dispensingActive &&
    digitalRead(START_PIN) == LOW)
{
    digitalWrite(DISPENSE_PIN, LOW);

    updateDispensingStatus(
        toiletDetailsId,
        0
    );

    displayInitialMessage();

    orderInProgress = false;

    dispensingActive = false;
}

  
    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n'); // Read the command until newline
        
        int separator1 = command.indexOf(',');
        int separator2 = command.indexOf(',', separator1 + 1);

        if (separator1 > 0 && separator2 == -1) { // Handling "GetAmountDetails"
            String cmd = command.substring(0, separator1);
            String toiletDetailsId = command.substring(separator1 + 1);

            if (cmd.equalsIgnoreCase("GetAmountDetails")) {
                // Fetch details using API
                String response = fetchAmount(toiletDetailsId);

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
                    String amount = parseJsonValue(response, "Amount");
                    if (amount != "") {
                        Serial.println("Fetched amount: " + amount);

                        // Optional: Perform additional actions with the fetched details
                        // For example, passing the amount to `createOrder`
                        createOrder(amount.toInt(), toiletDetailsId);
                    } else {
                        Serial.println("Failed to parse amount from response.");
                    }
                } else {
                    Serial.println("Failed to fetch details.");
                }
            } else {
                Serial.println("Unknown command.");
            }
        } else if (separator1 > 0 && separator2 > separator1) { // Handling "create order"
            String cmd = command.substring(0, separator1);
            String machine_id = command.substring(separator1 + 1, separator2);
            int amount = command.substring(separator2 + 1).toInt();

            if (cmd.equalsIgnoreCase("create order")) {
                showPaymentRequestUI();
                createOrder(amount, machine_id);
            }
        } else {
            Serial.println("Invalid input format. Use:");
            Serial.println("1. GetAmountDetails,toiletDetailsId");
            Serial.println("2. create order,machine_id,amount");
        }
    }
}

String parseJsonValue(String jsonString, String key) {
    StaticJsonDocument<200> doc;  // Adjust the size based on your response size
    DeserializationError error = deserializeJson(doc, jsonString);
    
    if (error) {
        Serial.println("Failed to parse JSON");
        return "";
    }

    // Extract the value for the given key
    if (doc.containsKey(key)) {
        return doc[key].as<String>();  // Return the value as String
    } else {
        Serial.println("Key not found: " + key);
        return "";
    }
}

void GetAmountDetails(){
               String response = fetchAmount(toiletDetailsId);

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

String updateDispensingStatus(String toiletDetailsId, int status) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String updateStatusAPI = "https://pen0xi2jmc.execute-api.eu-north-1.amazonaws.com/Production/updateStatus"; 
    
    // Create the payload with the 'body' field like the working test data
    String payload = "{\"body\": \"{\\\"ToiletDetailsID\\\": \\\"" + toiletDetailsId + "\\\", \\\"Status\\\": " + String(status) + "}\"}";

    http.begin(updateStatusAPI);
    http.addHeader("Content-Type", "application/json");

    // Print the payload for debugging
    Serial.println("Sending payload: " + payload);

    int httpCode = http.POST(payload); // Make the POST request

    if (httpCode == 200) {
      String response = http.getString(); // Get response if successful
      Serial.println("Toilet status updated: " + response);
      http.end();
      return response;
    } else {
      Serial.println("Error updating Toilet status: " + String(httpCode));
      http.end();
      return "ERROR";
    }
  } else {
    Serial.println("Wi-Fi not connected. Not able to update the Toilet status");
    return "ERROR";
  }
}



