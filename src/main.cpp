#include <Arduino.h>
#include <bluefruit.h>
#include <SPI.h>
// EPD
#include "Display_EPD_W21_spi.h"
#include "Display_EPD_W21.h"
// GUI
#include "GUI_Paint.h"
#include "fonts.h"
// other
#include "home_assistant.h"
#include "epaper.h"
#include <ctime>

// battery level
#define VREF 2.4
#define ADC_MAX 4096

// Refresh interval of HA data and display
const size_t TIME_REFRESH = 5 * 60 * 1000;

// Retry interval if no peripheral (server) is found
const size_t TIME_RETRY_SCAN = 15 * 1000;

// Advertising/Central parameters should have a global scope. Do NOT define them in 'setup' or in 'loop'
BLEClientService service("4a38ff83-3e18-4f35-a51b-90829dc07ed0");

// we use 4 characteristics in order to fit the whole HA JSON data
// as we are limited to 247 bytes per characteristic
// see https://devzone.nordicsemi.com/f/nordic-q-a/35927/max-data-length-over-ble
BLEClientCharacteristic characteristic1("048df6ac-7c4c-4383-897e-760bae10e321");
BLEClientCharacteristic characteristic2("0a555305-d8f6-433b-8833-67b4d1f38630");
BLEClientCharacteristic characteristic3("1dff0906-83c3-46ad-8da3-b999eba26e9b");
BLEClientCharacteristic characteristic4("dd511cd7-51b8-472b-aff7-183bc6cbfdf1");

// Handle for current connection
BLEConnection *connection;

// forward declarations
void writeSerial(String message, bool newLine = true);
void scan_callback(ble_gap_evt_adv_report_t *report);
void connect_callback(uint16_t conn_handle);
void disconnect_callback(uint16_t conn_handle, uint8_t reason);
void initDisplay();
void startScan();
void hibernateDisplay();
void writeDisplayData(char text[]);
float getBatteryVoltage();

int serial_enabled = 0;
size_t refresh_count = 0;
size_t scan_count = 0;
bool shouldUpdate = true;
#define DATA_TOKENS 3

#if 1
unsigned char BlackImage[EPD_ARRAY]; // Define canvas space
#endif

void setup()
{

    pinMode(D5, INPUT);  // BUSY
    pinMode(D0, OUTPUT); // RES
    pinMode(D3, OUTPUT); // DC
    pinMode(D1, OUTPUT); // CS

    // SPI
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
    SPI.begin();
    Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, 0, WHITE); // Set canvas parameters, GUI image rotation, please change 270 to 0/90/180/270.
    Paint_SelectImage(BlackImage);

    serial_enabled = bitRead(NRF_POWER->USBREGSTATUS, 0); // VBUSDETECT - USB supply status
    if (serial_enabled == 1)
    {
        Serial.begin(9600);
        while (!Serial)
            ;
        serial_enabled = 1;
    }

    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);

    if (!Bluefruit.begin(0, 1))
    {
        writeSerial("failed to initialize BLE!");
        return;
    }
    service.begin();

    characteristic1.begin();
    characteristic2.begin();
    characteristic3.begin();
    characteristic4.begin();

    // power management
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);

    // battery level
    analogReference(AR_INTERNAL_2_4);
    analogReadResolution(ADC_RESOLUTION);
    pinMode(PIN_VBAT, INPUT);
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);

    // init display
    // initDisplay();
    writeSerial("setup completed");

    startScan();
}

void loop()
{
    serial_enabled = bitRead(NRF_POWER->USBREGSTATUS, 0);

    if (Bluefruit.Scanner.isRunning())
    {
        writeSerial("Scanning (", false);
        writeSerial(String(scan_count) + ")");
        scan_count++;

        if (scan_count > 150)
        {
            writeSerial("Stopping Scanner");
            Bluefruit.Scanner.stop();
            delay(500);
            scan_count = 0;
            return;
        }
        delay(100);
    }
    else if (Bluefruit.Central.connected())
    {
        connection->requestPHY();
        connection->requestDataLengthUpdate();
        connection->requestMtuExchange(BLE_GATT_ATT_MTU_MAX);
        delay(500);

        char peer_name[32] = {0};
        connection->getPeerName(peer_name, sizeof(peer_name));

        writeSerial("Connected to ", false);
        writeSerial(peer_name, false);
        writeSerial(" with max Mtu: ", false);
        writeSerial(String(connection->getMtu()));

        service.discover(connection->handle());
        if (!service.discovered())
        {
            writeSerial("Service not discovered. Trying again.");
            connection->disconnect();
            delay(500);
            startScan();
            return;
        }
        characteristic1.discover();
        characteristic2.discover();
        characteristic3.discover();
        characteristic4.discover();

        const size_t CHARACTERISTIC_MAX_DATA_LEN = connection->getMtu() - 3;
        char buffer[1 * CHARACTERISTIC_MAX_DATA_LEN] = {0};
        char *current_pos = buffer;

        size_t bytes_received = characteristic1.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;
        //bytes_received = characteristic2.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        //current_pos += bytes_received;
        //bytes_received = characteristic3.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        //current_pos += bytes_received;
        /*bytes_received = characteristic4.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;*/
        memset(current_pos, 0, buffer + sizeof(buffer) - current_pos);
        Bluefruit.disconnect(connection->handle());

        String data = String(buffer);
        writeSerial(String("data: " + data));
        char *tokens[DATA_TOKENS]; // Array to store token pointers
        char *token;
        int index = 0;
    
        // Use strtok to split the string
        token = strtok(buffer, ",");
        while (token != NULL && index < DATA_TOKENS) {
            tokens[index] = token;  // Store pointer to token
            index++;
            token = strtok(NULL, ",");
        }


        if(token[0] == '1') {
            writeDisplayData(tokens);
        }
        refresh_count++;
        delay(TIME_REFRESH);
        startScan();
    }
    else
    {
        writeSerial("Nothing found during BLE scan. Trying again in some seconds.");
        delay(TIME_RETRY_SCAN);
        startScan();
    }
}

void startScan()
{
    Bluefruit.setName("SPServer");
    Bluefruit.autoConnLed(false);
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterUuid(service.uuid);
    Bluefruit.Scanner.start(0); // Scan timeout in 10 ms units
    delay(100);
}

void writeSerial(String message, bool newLine)
{
    if (serial_enabled == 1)
    {
        if (newLine == true)
        {
            Serial.println(message);
        }
        else
        {
            Serial.print(message);
        }
    }
}

void initDisplay()
{
    pinMode(D5, INPUT);  // BUSY
    pinMode(D0, OUTPUT); // RES
    pinMode(D3, OUTPUT); // DC
    pinMode(D1, OUTPUT); // CS

    // SPI
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
    SPI.begin();
}

void scan_callback(ble_gap_evt_adv_report_t *report)
{
    writeSerial("Connecting...");
    Bluefruit.Central.connect(report);
}

void connect_callback(uint16_t conn_handle)
{
    writeSerial("Connected");
    connection = Bluefruit.Connection(conn_handle);
    Bluefruit.Scanner.stop();
    delay(100);
    writeSerial("set connection to handle");
    // FIXME: move characteristic read + display update here?
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    (void)reason;

    writeSerial("Disconnected");
}

void writeDisplayData(char *tokens[DATA_TOKENS])
{
    float batteryVoltage = getBatteryVoltage();
    char c[50]; //size of the number
    snprintf(c, 50, "%f", batteryVoltage);
    EPD_HW_Init_GUI();                                       // GUI initialization.
    Paint_Clear(WHITE);                                      // Clear canvas.
    //Draw Todays Date
    Paint_DrawString_EN(0, 45, "Date: ", &Font20, WHITE, BLACK); 
    Paint_DrawString_EN(80, 45, tokens[1], &Font20, WHITE, BLACK);
    //Draw outdoor temp
    Paint_DrawString_EN(0, 90, "Temp Ute: ", &Font12, WHITE, BLACK); 
    Paint_DrawString_EN(100, 90, tokens[1], &Font12, WHITE, BLACK);
    //Draw Battery
    Paint_DrawString_EN(0, 250, "Batt: ", &Font12, WHITE, BLACK); 
    Paint_DrawString_EN(40,250, c, &Font12, WHITE, BLACK);
    EPD_Display(BlackImage);                                 // Display GUI image.
    EPD_DeepSleep();
}

float getBatteryVoltage()
{
    unsigned int adcCount = analogRead(PIN_VBAT);
    float adcVoltage = adcCount * VREF / ADC_MAX;
    return ((510e3 + 1000e3) / 510e3) * adcVoltage;
}
