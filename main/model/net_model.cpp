// Wi-Fi diagnostic text. See net_model.h for the contract.
//
// Moved verbatim from app_main.cpp. Pure model TU: standard library only.

#include "net_model.h"

const char *net_wifi_disconnect_reason_text(uint16_t reason)
{
    switch (reason) {
    case 0:
        return "none";
    case 2:
        return "auth-expire";
    case 3:
        return "auth-leave";
    case 4:
        return "assoc-expire";
    case 5:
        return "assoc-too-many";
    case 6:
        return "not-authenticated";
    case 7:
        return "not-associated";
    case 8:
        return "assoc-leave";
    case 15:
        return "4way-timeout";
    case 16:
        return "group-key-timeout";
    case 23:
        return "802.1x-auth-failed";
    case 200:
        return "beacon-timeout";
    case 201:
        return "no-ap-found";
    case 202:
        return "auth-failed";
    case 203:
        return "assoc-failed";
    case 204:
        return "handshake-timeout";
    case 205:
        return "connection-failed";
    default:
        return "unknown";
    }
}
