#include "ota/ota_ble.h"

#include <Arduino.h>
#include <BLECharacteristic.h>
#include <BLE2902.h>
#include <Update.h>

extern TaskHandle_t gatherTaskHandle;

/* ── UUIDs ──────────────────────────────────────────────────────── */
#define CHAR_OTA_CTRL_UUID  "12345678-1234-1234-1234-123456789ac0"
#define CHAR_OTA_DATA_UUID  "12345678-1234-1234-1234-123456789ac1"

/* ── Chunk queue config ─────────────────────────────────────────── */
#define OTA_QUEUE_SIZE  16
#define OTA_CHUNK_MAX   256

typedef struct {
        uint8_t data[OTA_CHUNK_MAX];
        size_t  len;  /* len == 0 means END sentinel */
} ota_chunk_t;

/* ── State ──────────────────────────────────────────────────────── */
static size_t        ota_total       = 0;
static size_t        ota_written     = 0;
static volatile bool ota_active      = false;
static QueueHandle_t ota_chunk_queue = NULL;
static TaskHandle_t  ota_write_task  = NULL;

/* ── Forward declarations ───────────────────────────────────────── */
static void ota_abort(const char *reason);
static void ota_finish(void);

/* ── Writer task — runs on core 1 ───────────────────────────────── */
static void IRAM_ATTR
ota_writer_task(void *arg)
{
        ota_chunk_t chunk;
        for (;;) {
                if (xQueueReceive(ota_chunk_queue, &chunk,
                                  portMAX_DELAY) != pdTRUE)
                        continue;

                /* Sentinel: END command received */
                if (chunk.len == 0) {
                        ota_finish();
                        vTaskDelete(NULL);
                        return;
                }

                size_t written = Update.write(chunk.data, chunk.len);
                if (written != chunk.len) {
                        Serial.printf("[OTA] Write error: %s\n",
                                      Update.errorString());
                        ota_abort("write error");
                        vTaskDelete(NULL);
                        return;
                }
                ota_written += written;
                Serial.printf("[OTA] %u / %u bytes\n",
                              ota_written, ota_total);
        }
}

/* ── Helpers ────────────────────────────────────────────────────── */
static void
ota_abort(const char *reason)
{
        ota_active = false;
        Update.abort();
        ota_total   = 0;
        ota_written = 0;
        if (ota_chunk_queue != NULL) {
                vQueueDelete(ota_chunk_queue);
                ota_chunk_queue = NULL;
        }
        if (gatherTaskHandle != NULL)
                vTaskResume(gatherTaskHandle);
        Serial.printf("[OTA] Aborted: %s\n", reason);
}

static bool
ota_begin(size_t total)
{
        if (gatherTaskHandle != NULL)
                vTaskSuspend(gatherTaskHandle);

        if (!Update.begin(total)) {
                Serial.printf("[OTA] Update.begin failed: %s\n",
                              Update.errorString());
                if (gatherTaskHandle != NULL)
                        vTaskResume(gatherTaskHandle);
                return false;
        }

        ota_chunk_queue = xQueueCreate(OTA_QUEUE_SIZE,
                                       sizeof(ota_chunk_t));
        if (ota_chunk_queue == NULL) {
                Serial.println("[OTA] Queue creation failed");
                Update.abort();
                if (gatherTaskHandle != NULL)
                        vTaskResume(gatherTaskHandle);
                return false;
        }

        /* Pin writer to core 1 — BLE GATT runs on core 0 */
        BaseType_t ret = xTaskCreatePinnedToCore(
                ota_writer_task, "ota_writer",
                8192, NULL, 5,
                &ota_write_task, 1);

        if (ret != pdPASS) {
                Serial.println("[OTA] Writer task creation failed");
                vQueueDelete(ota_chunk_queue);
                ota_chunk_queue = NULL;
                Update.abort();
                if (gatherTaskHandle != NULL)
                        vTaskResume(gatherTaskHandle);
                return false;
        }

        ota_total   = total;
        ota_written = 0;
        ota_active  = true;
        Serial.printf("[OTA] Started, expecting %u bytes\n", total);
        return true;
}

static void
ota_finish(void)
{
        if (ota_written != ota_total) {
                Serial.printf("[OTA] Size mismatch: %u / %u\n",
                              ota_written, ota_total);
                ota_abort("size mismatch");
                return;
        }
        if (!Update.end(true)) {
                Serial.printf("[OTA] Update.end failed: %s\n",
                              Update.errorString());
                ota_abort("end() error");
                return;
        }
        Serial.println("[OTA] Success — rebooting");
        Serial.flush();
        delay(300);
        ESP.restart();
}

/* ── Control characteristic callback ───────────────────────────── */
class OtaCtrlCallbacks : public BLECharacteristicCallbacks
{
        void onWrite(BLECharacteristic *ch) override
        {
                String cmd = ch->getValue().c_str();
                Serial.printf("[OTA] ctrl: %s\n", cmd.c_str());

                if (cmd.startsWith("START:")) {
                        size_t total = (size_t)cmd.substring(6).toInt();
                        if (total == 0) {
                                Serial.println("[OTA] Invalid size");
                                return;
                        }
                        if (ota_active)
                                ota_abort("restarted by client");
                        ota_begin(total);

                } else if (cmd == "END") {
                        if (!ota_active) {
                                Serial.println("[OTA] END without active session");
                                return;
                        }
                        /* Send sentinel to writer task */
                        ota_chunk_t sentinel;
                        memset(&sentinel, 0, sizeof(sentinel));
                        sentinel.len = 0;
                        xQueueSend(ota_chunk_queue, &sentinel,
                                   portMAX_DELAY);
                        ota_active = false;

                } else if (cmd == "ABORT") {
                        ota_abort("client request");

                } else {
                        Serial.printf("[OTA] Unknown command: %s\n",
                                      cmd.c_str());
                }
        }
};

/* ── Data characteristic callback ───────────────────────────────── */
class OtaDataCallbacks : public BLECharacteristicCallbacks
{
        void onWrite(BLECharacteristic *ch) override
        {
                if (!ota_active || ota_chunk_queue == NULL) {
                        Serial.println("[OTA] Data without active session");
                        return;
                }

                std::string val = ch->getValue();
                size_t      len = val.size();

                if (len > OTA_CHUNK_MAX) {
                        Serial.printf("[OTA] Chunk too large: %u\n", len);
                        ota_abort("chunk too large");
                        return;
                }

                ota_chunk_t chunk;
                memcpy(chunk.data, val.data(), len);
                chunk.len = len;

                /* 500ms timeout — if queue full, ESP32 is overwhelmed */
                if (xQueueSend(ota_chunk_queue, &chunk,
                               pdMS_TO_TICKS(500)) != pdTRUE) {
                        Serial.println("[OTA] Queue full — aborting");
                        ota_abort("queue full");
                }
        }
};

/* ── Public init ─────────────────────────────────────────────────── */
void
ota_ble_init(BLEService *service)
{
        BLECharacteristic *ctrl = service->createCharacteristic(
                CHAR_OTA_CTRL_UUID,
                BLECharacteristic::PROPERTY_WRITE);
        ctrl->setCallbacks(new OtaCtrlCallbacks());

        BLECharacteristic *data = service->createCharacteristic(
                CHAR_OTA_DATA_UUID,
                BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_WRITE_NR);
        data->setCallbacks(new OtaDataCallbacks());

        Serial.println("[OTA] BLE characteristics registered");
}

bool
ota_ble_is_active(void)
{
        return ota_active;
}