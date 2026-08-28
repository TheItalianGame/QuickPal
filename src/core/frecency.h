#pragma once

#include "types.h"

void loadFrecency();
int frecencyBonus(const Command& command);
void recordCommandLaunch(const Command& command);

