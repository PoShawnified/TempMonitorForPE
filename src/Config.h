#ifndef Config_H
#define Config_H

#include "Arduino.h"

class WiFiConfig {
    public:
        WiFiConfig();
        ~WiFiConfig();
        char* ssid();
        char* password();

    private:
        char* _ssid;
        char* _password;
};

class HttpsConfig {
    public:
        HttpsConfig();
        ~HttpsConfig();
        String apikey();
        String iftttalert();
        String iftttnotification();
        char* fingerprint();

    private:
        String _apikey;
        String _iftttalert;
        String _iftttnotification;
        char* _fingerprint;
};

#endif