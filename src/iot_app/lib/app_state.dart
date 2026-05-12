import 'dart:async';
import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class AppState extends ChangeNotifier {
  // --- VARIABILI GPS ---
  double latitude = 0.0;
  double longitude = 0.0;
  double speedKmH = 0.0;
  StreamSubscription<Position>? _positionStream;

  // --- VARIABILI BLUETOOTH ---
  bool isScanning = false;
  List<ScanResult> scanResults = [];
  BluetoothDevice? connectedDevice;
  String bleData = "Nessun dato";
  StreamSubscription<List<int>>? _bleNotifySub;

  AppState() {
    _initGpsStream(); // Avvia il GPS appena l'app si apre
  }

  // ==========================================
  // LOGICA GPS
  // ==========================================
  Future<void> _initGpsStream() async {
    try {
      // 1. Controlla i permessi (Qui su PC andrà in errore e salterà al blocco catch)
      LocationPermission permission = await Geolocator.checkPermission();
      if (permission == LocationPermission.denied) {
        permission = await Geolocator.requestPermission();
        if (permission == LocationPermission.denied) return;
      }

      // 2. Iscrizione al flusso continuo (Stream) del GPS
      _positionStream = Geolocator.getPositionStream(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          distanceFilter: 1, // Aggiorna ogni metro
        ),
      ).listen((Position position) {
        latitude = position.latitude;
        longitude = position.longitude;
        speedKmH = position.speed * 3.6; 
        
        notifyListeners(); 
      });

    } catch (e) {
      // SALVAGENTE: Se non trova l'hardware (siamo su PC), carica dati finti!
      print("Sensore GPS non trovato. Carico dati finti per testare la UI.");
      latitude = 41.8902;  // Mettiamo le coordinate di Roma
      longitude = 12.4922;
      speedKmH = 45.5;     // Una velocità finta a caso
      
      notifyListeners(); // Mostra l'interfaccia comunque
    }
  }

  // ==========================================
  // LOGICA BLUETOOTH (BLE)
  // ==========================================
  Future<void> startBleScan() async {
    isScanning = true;
    scanResults.clear();
    notifyListeners();

    // Ascolta i risultati dello scan
    var subscription = FlutterBluePlus.onScanResults.listen((results) {
      scanResults = results;
      notifyListeners();
    });

    // Avvia scansione per 5 secondi
    await FlutterBluePlus.startScan(timeout: const Duration(seconds: 5));
    
    // Fine scansione
    isScanning = false;
    notifyListeners();
  }

  Future<void> connectToDevice(
    BluetoothDevice device, {
    Guid? serviceUuid,
    Guid? characteristicUuid,
  }) async {
    try {
      await device.connect();
      connectedDevice = device;
      bleData = "Connesso all'ESP32!";
      notifyListeners();
      if (serviceUuid != null && characteristicUuid != null) {
        await startBleStringNotifications(
          serviceUuid: serviceUuid,
          characteristicUuid: characteristicUuid,
        );
      }

    } catch (e) {
      bleData = "Errore di connessione";
      notifyListeners();
    }
  }

  Future<void> startBleStringNotifications({
    required Guid serviceUuid,
    required Guid characteristicUuid,
  }) async {
    final device = connectedDevice;
    if (device == null) {
      bleData = "Nessun dispositivo connesso";
      notifyListeners();
      return;
    }

    try {
      final services = await device.discoverServices();
      final service = services.firstWhere(
        (s) => s.uuid == serviceUuid,
        orElse: () => throw Exception("Servizio non trovato"),
      );
      final characteristic = service.characteristics.firstWhere(
        (c) => c.uuid == characteristicUuid,
        orElse: () => throw Exception("Caratteristica non trovata"),
      );

      await characteristic.setNotifyValue(true);
      await _bleNotifySub?.cancel();
      _bleNotifySub = characteristic.onValueReceived.listen((bytes) {
        bleData = String.fromCharCodes(bytes);
        notifyListeners();
      });
    } catch (e) {
      bleData = "Errore ricezione BLE";
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _positionStream?.cancel(); // Pulisce la RAM quando l'app si chiude
    _bleNotifySub?.cancel();
    super.dispose();
  }
}