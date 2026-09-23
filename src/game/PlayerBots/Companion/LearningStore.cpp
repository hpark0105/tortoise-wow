// BL-003 (KAP-558): the FIFO and the Store are header-inline on purpose:
// src/game/CMakeLists.txt names the PlayerBot sources explicitly (no
// glob) and CMake is outside the BL-003 edit set, so every referenced
// symbol must be visible to the already listed translation units.
// This translation unit keeps the header compilable as a standalone
// unit; it defines no symbols of its own.
#include "Companion/LearningStore.h"
