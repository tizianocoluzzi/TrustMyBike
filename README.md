# TrustMyBike
project for IoT algorithm and Services course in Sapienza
## Group
Tiziano Coluzzi

Aldo Bosco

Alessandro Arellano Altuna
## System Description
### Goal
Producing a smart bike that can evaluate the quality of the road and create a map that can be shared across devices, 
allowing bikers to better understand road conditions in advance to decide the better path to go 
### Sensors
- Accelerometer: MPU6050 with the goal to identify holes, speed
- Gyrosope: evaluate slopeness of the street
- Integration with phone GPS   
### Power Source
The project aims to be powered by a dynamo for the bike's wheel. Electrical characteristic: 6V, 3W of max power.
#### Power consumption
| System | Current (max)| 
| -- | -- |
| accelerometer | 500 $\mu\text{A}$ |
| gyroscope | 3.6 mA |
| bluetooth | 130 mA |
| ESP | 50 mA |
| Total: | 187.1 mA |

The dynamo can genrate 500 mA
### Sampling
The sampling frequency depends on speed and space.
Given a medium wheel circumference of 70 cm, we define a bump as a hole with a 10 cm diamter.
So the sampling frequency follows this relation: $F = \frac{2 * \text{speed}}{ \text{bump length}}$ 

Obtained using Sampling theorem.

### Learning
From the collected data we aim to learn to classify the quality of the street and to detect hazards.

### Communication

```
MPU -> ESP <-> Phone(enrich with GPS) -> Cloud (for building and sharing the map)
        ^
        |
      dynamo
```
### Description
ESP compute classification task to undestand street quality, communicates with the phone who enrich the obtained data.
The obtained data are transimtted to the cloud to create this shared map.

### Youtube link for demo
https://youtube.com/@tizianocoluzzi?si=6QXLpHARwTKoehrB
