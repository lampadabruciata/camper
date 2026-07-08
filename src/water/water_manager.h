#ifndef WATER_MANAGER_H
#define WATER_MANAGER_H

// Salvataggio litri acqua su SD
void saveLastLitersToSD(float liters);

// Caricamento litri acqua da SD
float loadLastLitersFromSD();

#endif // WATER_MANAGER_H