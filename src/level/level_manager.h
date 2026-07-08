#ifndef LEVEL_MANAGER_H
#define LEVEL_MANAGER_H

// Dichiarazione della funzione
void captureLevelZero();
bool saveLevelZeroToSD(float roll0, float pitch0);
bool loadLevelZeroFromSD(float &roll0, float &pitch0);
bool clearLevelZeroOnSD();

#endif // LEVEL_MANAGER_H