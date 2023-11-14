#include <Arduino.h>
#include <iot.h>

void setup() {

    Serial.begin(115200);


    iot.setApi("iot.hostname.de", "https://iot.hostname.de");// Hostname, Base URL
    // iot.setCACert(my_certificate); //optional to set a custom CA certificate to trust
    iot.setProject("test");
    iot.setProvisioningToken("1234567890");
    iot.connectWifi(WIFI_SSID, WIFI_PASSWORD);
    iot.syncNtpTime();

}

void loop() {
    delay(1000);
}
