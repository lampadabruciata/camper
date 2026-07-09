#ifndef WEB_MANAGER_H
#define WEB_MANAGER_H

#include <WebServer.h>

// Dichiarazione funzione sendFile
void sendFile(const char* path, const char* mime);

// Handler dati acqua
void handleDataWater();

// Handler dati gas
void handleDataGas();

#endif // WEB_MANAGER_H