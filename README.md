# TrustMyBike

A final IoT project for road-surface quality monitoring during urban cycling. The system combines an embedded sensing node with a mobile application to collect motion and location data, infer a road-quality class, and publish batched geo-referenced observations for downstream services and visualization.

## Repository Links

- GitHub repository: https://github.com/tizianocoluzzi/TrustMyBike

## Media

- Demo video: https://youtube.com/shorts/E_YMKo_QBs8?si=Xx5bsStZRcIRPyXL

---

## Project Overview

TrustMyBike addresses the problem of monitoring road-surface quality from the perspective of cyclists, with the goal of associating sensed motion patterns and position data to a discrete road-quality score.

The implemented system is centered on a single embedded sensing platform and a companion mobile application. This project approach prioritize effort, reasoning, and justification of design choices more than maximizing the number of sensors or actuators.

### Core Idea

- Sense bike motion on-board.
- Estimate a road-quality class on the embedded device.
- Forward road-quality observations to a mobile application over BLE.
- Associate observations with GPS coordinates on the phone.
- Publish batched data to an MQTT broker for external consumption.

---

## Concept

### Problem Statement

Cyclists experience strong differences in comfort and safety depending on the road surface. A practical IoT service can support road monitoring by classifying surface quality along traveled paths and attaching these classifications to geographic coordinates.

### Objective

The project objective is to build and demonstrate an IoT pipeline that:

- acquires motion-related measurements on an embedded node,
- performs road-quality inference,
- reconnects robustly after low-power operation,
- transfers inferred results to a phone,
- georeferences them with GPS,
- and publishes them as a networked service.

### Intended Service

The service offered by the system is the generation of geo-referenced road-quality points. In the current implementation, points are accumulated on the mobile side and published in batches of 10 records after pairing each inferred score with latitude, longitude, and timestamp.

### Scope and Constraints

The available project material explicitly identifies the following targets and achieved constraints:

- [x] Total power less than or equal to 6 W
- [x] Time duration of 10 h plus 14 h deep sleep.
- [x] Reconnection latency of 1 s.
- [ ] Machine-learning accuracy target of 80%, with QWK reported as 80%: not accomplished due to the low numbered dataset.
- [x] Sampling frequency established at 50 Hz via FFT.

---

## Use Case

### Operational Scenario

A cyclist rides in an urban environment while the embedded node measures motion data and velocity-related information. The node estimates road quality and advertises the result over BLE; the phone application connects, receives notifications, adds GPS coordinates, and publishes batches to MQTT.

---

## System Architecture

The project consists of three main layers:

1. An embedded board based on an ESP32-class platform.
2. A Flutter mobile application.
3. A network service layer using MQTT.

<img width="745" height="490" alt="image" src="https://github.com/user-attachments/assets/0162538d-4030-466e-b9af-1ad1643ca060" />

### Architectural Blocks

| Layer | Implemented role | Evidence |
|---|---|---|
| Embedded node | Sensor acquisition, local inference, BLE peripheral, optional SD logging | `TrustMyBikeBoard` codebase, PlatformIO project, BLE service/characteristics, ML inference headers |
| Mobile app | BLE central, GPS acquisition, UI, MQTT publisher, local batching | Flutter `AppState`, `HomeScreen`, MQTT and GPS logic |
| Network layer | Remote message publication through MQTT | Use of `test.mosquitto.org`, topic `trustmybike/roadqualitybatch` |

### Data Flow

1. The embedded node samples motion data at 50 Hz.
2. Embedded software performs road-quality inference and exposes results through BLE notifications.
3. The mobile app scans for the device, connects, subscribes to notifications, and parses the received score.
4. The app associates each score with GPS latitude, longitude, and timestamp.
5. Every 10 collected points, the app serializes the batch to JSON and publishes one MQTT message.


---

## Hardware

### Embedded Platform

The board-side project is configured for `heltec_wifi_lora_32_V3` in PlatformIO, indicating an ESP32-based Heltec development board as the embedded platform.

### Sensors and Peripherals Found in the Codebase

| Component | Role in project | Evidence in code |
|---|---|---|
| MPU6050 | Motion sensing for acceleration and gyroscope data | `Adafruit MPU6050`, `readAccelGyro`, `mpu_data_t` with `ax, ay, az, gx, gy, gz` |
| Hall sensor | Speed or frequency sensing | `HallSensor`, `hallISR`, frequency and speed methods |
| INA219 | Power-related measurement support | `Adafruit INA219` include in embedded main file |
| OLED display | Local status display | `Heltec.display` usage in display module |
| SD card interface | Optional local CSV storage | `sd.h`, SPI pins, counter-based filenames |

<img width="1536" height="2048" alt="WhatsApp Image 2026-05-27 at 14 06 54" src="https://github.com/user-attachments/assets/1b16a918-7588-4db9-8b3a-616ecdf91b79" />
### Declared Board-Level Details

The embedded headers define:

- I2C pins: SDA 48, SCL 47.
- SPI pins: SCK 7, MISO 6, MOSI 5, CS 4. (used for data collection with an sdcard)
- MPU address: `0x68`.

---

## Software Stack

### Embedded Software

The embedded software is implemented with PlatformIO and the Arduino framework. The project depends on libraries for the MPU6050, INA219, TensorFlow Lite for ESP32, and Heltec board support.

### Mobile Application

The mobile application is implemented in Flutter. The app uses:

- `flutter_blue_plus` for BLE scanning and connection.
- `geolocator` for GPS.
- `mqtt_client` for MQTT publication.
- `provider` for state management.

### Machine Learning Pipeline

The repository includes training scripts and exported model artifacts. The ML code indicates:

- a 5-class ordinal classification problem,
- a motion branch using 64-sample windows,
- a velocity-feature branch,
- TensorFlow/Keras training,
- TensorFlow Lite export for deployment on the embedded device.

---

## IoT Communication

### BLE Interface

The embedded node exposes a BLE service with UUID `12345678-1234-1234-1234-123456789abc` and at least one characteristic used by the mobile app for road-quality notifications, with UUID `12345678-1234-1234-1234-123456789ab0`.

The mobile app supports scanning, connecting, enabling notifications, and handling reconnection after disconnection. The app logic explicitly mentions disconnection due to deep sleep and re-enables notifications when the device reconnects.

### MQTT Interface

The mobile application connects to the public broker `test.mosquitto.org` on port 1883 and publishes JSON batches to topic `trustmybike/roadqualitybatch`.

Each published record contains:

- latitude,
- longitude,
- score,
- timestamp.

---

## Service Definition

### Offered Service

The implemented IoT service is a road-quality data publishing pipeline. Its output is a sequence of geo-referenced road points, each associated with a discrete score from 1 to 5, produced by embedded inference and enriched by smartphone GPS.

### Current Output Representation

On the mobile side, the road-quality indicator is mapped to five labels:

- 1: poor / bumps or unpaved.
- 2: low quality / uneven.
- 3: medium / cobblestones.
- 4: good / rough asphalt.
- 5: excellent / smooth asphalt.

These labels are part of the application UI and should be treated as the current semantic interpretation of the model outputs.

---

## Algorithms

### Signal Acquisition

The embedded code defines a sampling frequency of 50 Hz and a fixed sampling interval derived from that value.


[INSERT EVALUATION GRAPH]

### Inference Pipeline

The ML inference interface specifies:

- window size: 196 samples,
- stride: 16 samples,
- 5 output classes,
- 6 motion features per sample (`ax, ay, az, gx, gy, gz`),
- 4 velocity-window features.

The training pipeline computes ordinal metrics including accuracy, expected-score MAE, linear weighted kappa, quadratic weighted kappa, confusion matrix, and per-class accuracy.

### Batching Logic

The phone application buffers inferred points locally. When the list reaches 10 points, it serializes the full batch to JSON and publishes one MQTT message, then clears the local buffer.

---

## Design

### Embedded Design

The embedded node integrates sensing, local processing, and BLE communication in a compact architecture. The source tree shows dedicated modules for display handling, ML inference, MPU sensing, hall sensing, and SD storage support.

### Mobile Design

The mobile app centralizes system state in a single `AppState` object. It manages:

- GPS stream initialization,
- BLE scan and connection,
- BLE notification subscription,
- MQTT client setup,
- local buffering of road points.

### Networked Design Choices

The design splits responsibilities across node and phone:

- the embedded side handles sensing and classification,
- the mobile side handles geolocation and internet connectivity.

This division is consistent with resource-aware IoT design, but the final Design document should explicitly justify why GPS and MQTT publication were assigned to the phone rather than the embedded board.

---

## Evaluation Setup

### Evaluation Dimensions

The project notes and codebase support evaluation along at least four dimensions:

- energy consumption,
- operating duration and deep sleep behavior,
- reconnection latency,
- machine-learning performance,
- sampling configuration.

### Reported Targets and Outcomes

| Dimension | Target / requirement | Reported outcome |
|---|---|---|
| Power | Total power <= 6 W | 120 mA at 3.3 V, about 369 mW |
| Duration | 10 h + 14 h deep sleep | 120 mA * 10h + 2mA * 14h = 1228 mAh <= 2000 mAh battery|
| Reconnection latency | 1 s | Accomplished |
| ML performance | Accuracy 80%, QWK 80% | Not accomplished for lack of data |
| Sampling frequency | Establish via FFT | 50 Hz reported |

### Methodology Notes

The repository contains explicit ML evaluation code and a saved training summary with validation metrics such as accuracy, weighted kappa, MAE, confusion matrix, and class-wise accuracy.

However, the exact mapping between the final reported headline values in the project notes and the specific saved training run in the repository is not fully documented. For that reason, the final Evaluation document should include:

- dataset description,
- train/validation split rationale,
- metric definitions,
- the exact experiment version used for the final reported numbers,
- raw plots and tables.

<img width="700" height="500" alt="CNN only vs fusion" src="https://github.com/user-attachments/assets/74a3f814-5db0-4508-8c66-50ac38517f1c" />

<img width="700" height="500" alt="confusion matrix" src="https://github.com/user-attachments/assets/19d28d71-6c6d-45af-82d0-4cc793dd2539" />

<img width="700" height="500" alt="CV metrics" src="https://github.com/user-attachments/assets/e55a512c-a57e-4315-bd07-7d47bedb2eea" />

---

## Results

### Reported Final Results

The project information explicitly reports the following final outcomes:

- power consumption: about 369 mW from 120 mA at 3.3 V,
- battery target achieved for 10 h plus 14 h deep sleep using a 2000 mA battery,
- reconnection latency: 1 s,
- ML accuracy: 50%,
- QWK: 70%,
- sampling frequency: 50 Hz.


<img width="1665" height="817" alt="Screenshot From 2026-05-26 16-49-05" src="https://github.com/user-attachments/assets/df2b1b75-416f-479b-9abe-37c474099660" />

<img width="602" height="415" alt="Screenshot From 2026-05-26 16-45-08" src="https://github.com/user-attachments/assets/e5332a96-a498-4f96-9e63-25900f3883cf" />


### Additional Repository Metrics

The training artifacts in the repository also expose validation metrics for at least one run, including:

- validation accuracy,
- expected-score MAE,
- linear weighted kappa,
- quadratic weighted kappa,
- within-1-class performance,
- confusion matrix,
- per-class accuracy.
---


## Repository Structure

```text
.
├── TrustMyBikeBoard/        # Embedded PlatformIO project
├── iotapp/                  # Flutter mobile application
├── training/                # Model training and export artifacts
└──
```

### Main Relevant Modules

| Path | Purpose |
|---|---|
| `TrustMyBikeBoard/src/main.cpp` | Embedded acquisition, BLE, and runtime logic |
| `TrustMyBikeBoard/include/mlinference.h` | Inference interface and ML window definitions |
| `TrustMyBikeBoard/include/sensors/mpu.h` | Motion sensor structures and functions |
| `TrustMyBikeBoard/include/sensors/hall.h` | Speed or frequency sensing |
| `iotapp/lib/appstate.dart` | GPS, BLE, MQTT, and batching logic |
| `iotapp/lib/homescreen.dart` | Dashboard UI |
| `training/` | Training scripts and saved summaries |

---

## Deployment and Demonstration

### Current Demonstration Path

A complete demonstration can be organized as follows:

1. Start the embedded board.
2. Open the mobile application.
3. Scan for the BLE device and connect.
4. Observe GPS, BLE status, and road-quality indicator in the dashboard.
5. Generate enough data points to trigger batch publication to MQTT.


