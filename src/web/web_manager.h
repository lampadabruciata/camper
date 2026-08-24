#ifndef WEB_MANAGER_H
#define WEB_MANAGER_H

#include <WebServer.h>

// Dichiarazione funzione sendFile
void sendFile(const char* path, const char* mime);

// Handler dati acqua
void handleDataWater();

// Handler dati gas
void handleDataGas();

// Handler dati live
void handleDataLive();

// Handler dati Ah 24h
void handleDataAh24h();

// Handler dati Ah live
void handleDataAhLive();

// Handler dati solare
void handleDataSolar();

// Handler dati batteria (grafico 24h)
void handleDataBattery();

// Handler pagine web statiche
void handleIndex();
void handleChartJs();

// Handler diagnostica reboot
void handleDataBoot();

#endif // WEB_MANAGER_H
