#ifndef GAS_MANAGER_H
#define GAS_MANAGER_H

// Salvataggio gas su SD
void saveGasToSD(const char* filename, float kg);

// Caricamento gas da SD
float loadGasFromSD(const char* filename);

#endif // GAS_MANAGER_H