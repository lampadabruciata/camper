#ifndef WATER_MANAGER_H
#define WATER_MANAGER_H

// Salvataggio litri acqua su SD
void saveLastLitersToSD(float liters);

// Caricamento litri acqua da SD
float loadLastLitersFromSD();

// Ripristina litri consumati da SD
void restoreLitersFromSD();

// Imposta capacità serbatoio acqua
void handleSetTank();

#endif // WATER_MANAGER_H
