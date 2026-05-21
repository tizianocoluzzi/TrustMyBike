import 'dart:async';
import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'dart:convert'; 
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'ota_service.dart';

class AppState extends ChangeNotifier {
  // --- VARIABILI GPS ---
  double latitude = 0.0;
  double longitude = 0.0;
  double speedKmH = 0.0;
  int roadQualityScore = 5; 
  List<RoadPoint> roadHistory = [];
  StreamSubscription<Position>? _positionStream;

  // --- VARIABILI BLUETOOTH ---
  bool isScanning = false;
  List<ScanResult> scanResults = [];
  BluetoothDevice? connectedDevice;
  String bleData = "Nessun dato";
  StreamSubscription<List<int>>? _bleDataSubscription; // Unico listener necessario

  // --- VARIABILI MQTT ---
  MqttServerClient? mqttClient;
  bool isMqttConnected = false;

  // --- VARIABILI OTA ---
  final OtaService otaService = OtaService();

  final String mlQueueServiceUuid = "12345678-1234-1234-1234-123456789abc";
  final String mlQueueCharacteristicUuid = "12345678-1234-1234-1234-123456789ab0";

  AppState() {
    _initGpsStream();
    _initMqtt(); 
  }

  // ==========================================
  // LOGICA GPS
  // ==========================================
  Future<void> _initGpsStream() async {
    try {
      LocationPermission permission = await Geolocator.checkPermission();
      if (permission == LocationPermission.denied) {
        permission = await Geolocator.requestPermission();
        if (permission == LocationPermission.denied) return;
      }

      _positionStream = Geolocator.getPositionStream(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          distanceFilter: 1, 
        ),
      ).listen((Position position) {
        latitude = position.latitude;
        longitude = position.longitude;
        speedKmH = position.speed * 3.6; 
        
        notifyListeners(); 
      });

    } catch (e) {
      print("Sensore GPS non trovato. Carico dati finti per testare la UI.");
      latitude = 41.8902;  
      longitude = 12.4922;
      speedKmH = 45.5;     
      
      notifyListeners(); 
    }
  }

  // ==========================================
  // LOGICA BLUETOOTH (BLE)
  // ==========================================
  Future<void> startBleScan() async {
    isScanning = true;
    scanResults.clear();
    notifyListeners();

    FlutterBluePlus.onScanResults.listen((results) {
      scanResults = results;
      notifyListeners();
    });

    await FlutterBluePlus.startScan(timeout: const Duration(seconds: 5));
    
    isScanning = false;
    notifyListeners();
  }

 Future<void> connectToDevice(BluetoothDevice device) async {
    try {
      // 1. Connessione iniziale (attiviamo l'autoConnect fin da subito)
      await device.connect(autoConnect: true);
      connectedDevice = device;

      // =======================================================
      // 2. CANE DA GUARDIA PER IL DEEP SLEEP (Auto-Reconnect)
      // =======================================================
      device.connectionState.listen((BluetoothConnectionState state) async {
        if (state == BluetoothConnectionState.disconnected) {
          print(" ESP32 in Deep Sleep o disconnesso...");
          bleData = "ESP in Deep Sleep. Attesa...";
          notifyListeners();

        } else if (state == BluetoothConnectionState.connected) {
          print(" Connessione BLE stabilita. Configuro i sensori...");
          bleData = "Connesso! Riattivazione sensori...";
          notifyListeners();

          // Riaccendiamo le notifiche a ogni risveglio!
          await _setupNotifications(device);
        }
      });

    } catch (e) {
      bleData = "Errore di connessione: $e";
      notifyListeners();
    }
  }

  // =======================================================
  // 3. FUNZIONE PER ACCENDERE L'INTERRUTTORE DEI DATI
  // =======================================================
  Future<void> _setupNotifications(BluetoothDevice device) async {
    try {
      List<BluetoothService> services = await device.discoverServices();
      
      for (BluetoothService service in services) {
        if (service.uuid.toString() == mlQueueServiceUuid) {
          for (BluetoothCharacteristic characteristic in service.characteristics) {
            
            if (characteristic.uuid.toString() == mlQueueCharacteristicUuid) {
              
              // 1. Riaccendiamo le notifiche
              await characteristic.setNotifyValue(true);
              print(" Iscrizione alla coda ML completata.");
              
              // 2. Stacchiamo le vecchie orecchie se c'erano (evita dati sdoppiati)
              await _bleDataSubscription?.cancel();
              
              // 3. Ascoltiamo il nuovo flusso di dati
              _bleDataSubscription = characteristic.lastValueStream.listen((List<int> value) {
                if (value.isEmpty) return;

                String stringaRicevuta = String.fromCharCodes(value).trim();
                int? parsedScore = int.tryParse(stringaRicevuta);
                
                if (parsedScore != null && parsedScore >= 1 && parsedScore <= 5) {
                  roadQualityScore = parsedScore;
                  bleData = "Qualità Strada: $roadQualityScore/5";
                  
                  RoadPoint nuovoPunto = RoadPoint(
                    latitude: latitude,
                    longitude: longitude,
                    score: roadQualityScore,
                    timestamp: DateTime.now(),
                  );

                  roadHistory.add(nuovoPunto);
                  print(" Punto accumulato (Totale: ${roadHistory.length}/10)");

                  if (roadHistory.length >= 10) {
                    publishRoadPointsBatch();
                  }

                  notifyListeners(); 
                } else {
                  print("Dato sporco ignorato: $stringaRicevuta");
                }
              });
            }
          }
        }
      }
    } catch (e) {
      print("Errore durante il setup delle notifiche: $e");
    }
  }

  // Un solo metodo dispose per pulire tutto quando l'app si chiude
  @override
  void dispose() {
    _positionStream?.cancel(); 
    _bleDataSubscription?.cancel(); 
    otaService.dispose();
    super.dispose();
  }

  Future<void> performOtaUpdate() async {
// Stop listening to road data during OTA
    await _bleDataSubscription?.cancel();
    _bleDataSubscription = null;

    await otaService.runOtaUpdate(connectedDevice);

    // Re-setup notifications after OTA (device will reboot anyway,
    // but good practice if OTA fails)
    if (connectedDevice != null) {
      await _setupNotifications(connectedDevice!);
    }

    notifyListeners();
  }

  // ==========================================
  // LOGICA MQTT (VERO BATCHING)
  // ==========================================
  Future<void> _initMqtt() async {
    mqttClient = MqttServerClient('broker.hivemq.com', 'flutter_bike_${DateTime.now().millisecondsSinceEpoch}');
    mqttClient!.port = 1883;
    mqttClient!.logging(on: false);
    mqttClient!.keepAlivePeriod = 20;

    try {
      await mqttClient!.connect();
      isMqttConnected = true;
      print(" Connesso al Broker MQTT con successo!");
      notifyListeners();
    } catch (e) {
      print(" Errore di connessione MQTT: $e");
      mqttClient!.disconnect();
    }
  }

  // Prende tutta la lista e la manda come UN SOLO messaggio
  void publishRoadPointsBatch() {
    if (mqttClient == null || mqttClient!.connectionStatus!.state != MqttConnectionState.connected) {
      print(" MQTT non connesso. I dati rimangono nel buffer locale (${roadHistory.length} punti).");
      return; 
    }

    if (roadHistory.isEmpty) return;

    // Trasforma i 10 punti in un array di JSON
    List<Map<String, dynamic>> jsonBatch = roadHistory.map((point) => {
      "latitude": point.latitude,
      "longitude": point.longitude,
      "score": point.score,
      "timestamp": point.timestamp.toIso8601String()
    }).toList();

    // Incolla tutto in una singola stringa testuale
    String payload = jsonEncode(jsonBatch);

    final builder = MqttClientPayloadBuilder();
    builder.addString(payload);

    mqttClient!.publishMessage(
      'trustmybike/road_quality_batch', 
      MqttQos.atLeastOnce,
      builder.payload!
    );
    
    print(" Unico Batch da ${roadHistory.length} punti inviato con successo via MQTT!");
    
    // Svuota la lista solo dopo aver inviato il pacco!
    roadHistory.clear(); 
    notifyListeners(); 
  }
}

// Modello per salvare la coordinata abbinata al voto della strada
class RoadPoint {
  final double latitude;
  final double longitude;
  final int score;
  final DateTime timestamp;

  RoadPoint({
    required this.latitude,
    required this.longitude,
    required this.score,
    required this.timestamp,
  });

  @override
  String toString() {
    return "Punto: ($latitude, $longitude) -> Voto: $score";
  }
}