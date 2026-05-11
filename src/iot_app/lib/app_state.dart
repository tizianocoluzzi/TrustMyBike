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

  Future<void> connectToDevice(BluetoothDevice device) async {
    try {
      await device.connect();
      connectedDevice = device;
      bleData = "Connesso all'ESP32!";
      notifyListeners();

      // Mettiamo il codice per LEGGERE i dati qui (Discover Services)
      // (Lo farai quando conoscerai gli UUID dell'ESP32 del tuo compagno)

    } catch (e) {
      bleData = "Errore di connessione";
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _positionStream?.cancel(); // Pulisce la RAM quando l'app si chiude
    super.dispose();
  }
}