# IoT_gorup_project
project for IoT algorithm and Services course in Sapienza
## System Description
### Goal
Producing a smart bike that can evaluate teh quality of the road and create a shared mab that can be shared across devices
### Sensors
- Accelerometer: MPU6050 with the goal to identify holes, speed, ...
- gyrosope: evaluate slopeness of the street
- (optional): integration with device GPS 
- (more optional): camera to perform evaluation using images  
### Energy harvesting
The project aims to be powered by a dynamo for the bike's wheel. Electrical characteristic: 6V, 3W of max power.
## Design
### Communication


MPU -> ESP <-> Phone(enrich with GPS) -> Cloud (for building and sharing the map)
        ^
        |
      dynamo

### Description
ESP compute classification task to undestand street quality, communicates with the phone who enrich the obtained data.
The obtained data are transimtted to the cloud to create this shared map.

### What we need to determine:
- frequency of sampling: idea it might depend on the speed
- frequency of transmission: The frequency of transmission determine the enrichment so it needs to be fats enougn to build a street but no need to transmit every sample
- data extraction: Which data do we care about? e.g. We can use sliding window to detect spikes 
- model: what model should we use to perfom the learning task? supervised/unsupervised?
- data: how do we collect data to have this samples? https://data.mendeley.com/datasets/3j9yh8znj4


