#pragma once

#include <Arduino.h>

namespace sd
{
bool init();
bool write_csv(const char *path, const char *line);
}