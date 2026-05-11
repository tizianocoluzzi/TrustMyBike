#include "sd/sd.h"

#include "FS.h"
#include "SD.h"
#include "SPI.h"

#include "board.h"

namespace
{
SPIClass spi(SPI);
}

namespace sd
{
bool init()
{
    spi.begin(SCK, MISO, MOSI, CS);

    if (!SD.begin(CS, spi, 4000000))
    {
        Serial.println("SD card init failed!");
        return false;
    }

    Serial.println("SD card ready.");

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE)
    {
        Serial.println("No SD card attached");
        return false;
    }

    Serial.print("SD Card Type: ");
    if (cardType == CARD_MMC)
    {
        Serial.println("MMC");
    }
    else if (cardType == CARD_SD)
    {
        Serial.println("SDSC");
    }
    else if (cardType == CARD_SDHC)
    {
        Serial.println("SDHC");
    }
    else
    {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    return true;
}

bool write_csv(const char *path, const char *line)
{
    File file = SD.open(path, FILE_APPEND);
    if (!file)
    {
        Serial.println("Failed to open CSV file");
        return false;
    }

    bool ok = file.println(line) > 0;
    file.close();
    return ok;
}

uint32_t read_counter()
{
    File file = SD.open("/counter.txt", FILE_READ);
    if (!file)
    {
        Serial.println("Counter file not found, initializing to 0");
        return 0;
    }

    char buffer[20];
    int len = file.readBytes(buffer, sizeof(buffer) - 1);
    file.close();

    if (len <= 0)
    {
        Serial.println("Counter file is empty, initializing to 0");
        return 0;
    }

    buffer[len] = '\0';
    uint32_t counter = atoi(buffer);
    Serial.printf("Read counter from SD: %lu\n", counter);
    return counter;
}

bool increment_counter()
{
    uint32_t counter = read_counter() + 1;
    
    File file = SD.open("/counter.txt", FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open counter file for writing");
        return false;
    }

    file.seek(0);
    file.print(counter);
    file.close();
    
    Serial.printf("Incremented counter to: %lu\n", counter);
    return true;
}
}
