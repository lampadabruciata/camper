#ifndef GAS_MANAGER_H
#define GAS_MANAGER_H

#include "HX711.h"

// Salvataggio gas su SD
void saveGasToSD(const char* filename, float kg);

// Caricamento gas da SD
float loadGasFromSD(const char* filename);

// Ripristino gas da SD
void restoreGasFromSD();

// Wrapper thread-safe per HX711
bool scaleSafeGetUnits(HX711 &scale, long samples, float &out);
bool scaleSafeTare(HX711 &scale);

#endif // GAS_MANAGER_H