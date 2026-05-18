import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'app_state.dart';

class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key});

  // =========================================================
  // WIDGET: INDICATORE QUALITÀ STRADA
  // =========================================================
  Widget _buildRoadQualityIndicator(int score) {
    Color cardColor;
    String statusText;
    IconData statusIcon;

    switch (score) {
      case 1:
        cardColor = Colors.red;
        statusText = "Pessima (Buche/Sterrato)";
        statusIcon = Icons.warning_amber_rounded;
        break;
      case 2:
        cardColor = Colors.orange;
        statusText = "Scarsa (Sconnessa)";
        statusIcon = Icons.moving_rounded;
        break;
      case 3:
        cardColor = Colors.yellow.shade700;
        statusText = "Media (Sanpietrini)";
        statusIcon = Icons.hail_rounded;
        break;
      case 4:
        cardColor = Colors.lightGreen;
        statusText = "Buona (Asfalto ruvido)";
        statusIcon = Icons.directions_bike_rounded;
        break;
      case 5:
      default:
        cardColor = Colors.green;
        statusText = "Ottima (Asfalto liscio)";
        statusIcon = Icons.speed_rounded;
        break;
    }

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: cardColor.withOpacity(0.2), // Sfondo semi-trasparente
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: cardColor, width: 2),
      ),
      child: Row(
        children: [
          Icon(statusIcon, color: cardColor, size: 40),
          const SizedBox(width: 16),
          // Expanded evita che i testi lunghi "rompano" lo schermo dei telefoni più stretti
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text("Qualità Manto Stradale", style: TextStyle(color: Colors.grey, fontSize: 12)),
                Text(
                  statusText, 
                  style: TextStyle(color: cardColor, fontSize: 18, fontWeight: FontWeight.bold)
                ),
              ],
            ),
          )
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    // Collega questa schermata al "Cervello" (AppState)
    final state = context.watch<AppState>();
    final serviceUuid = Guid("12345678-1234-1234-1234-123456789abc");
    final characteristicUuid = Guid("12345678-1234-1234-1234-123456789ab0");

    return Scaffold(
      appBar: AppBar(
        title: const Text("TrustMyBike Dashboard"), // Un titolo più professionale
        backgroundColor: Colors.blueAccent,
        foregroundColor: Colors.white, // Rende il titolo bianco e ben leggibile
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // --- 1. CARD DEL GPS ---
            Card(
              elevation: 4,
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  children: [
                    const Text("Dati GPS", style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold)),
                    const Divider(),
                    Text("Latitudine: ${state.latitude.toStringAsFixed(5)}"),
                    Text("Longitudine: ${state.longitude.toStringAsFixed(5)}"),
                    const SizedBox(height: 10),
                    Text(
                      "Velocità: ${state.speedKmH.toStringAsFixed(1)} km/h",
                      style: const TextStyle(fontSize: 24, color: Colors.blue),
                    ),
                  ],
                ),
              ),
            ),
            
            const SizedBox(height: 16),

            // --- 2. INDICATORE QUALITÀ STRADA ---
            // ECCOLO QUI! Ora è connesso al cervello dell'app (state.roadQualityScore)
            _buildRoadQualityIndicator(state.roadQualityScore),

            const SizedBox(height: 20),

            // --- 3. SEZIONE BLUETOOTH ---
            ElevatedButton.icon(
              onPressed: state.isScanning ? null : () => context.read<AppState>().startBleScan(),
              icon: state.isScanning 
                  ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)) 
                  : const Icon(Icons.bluetooth_searching),
              label: Text(state.isScanning ? "Scansione in corso..." : "Cerca ESP32"),
            ),

            const SizedBox(height: 10),
            Text("Stato BLE: ${state.bleData}", textAlign: TextAlign.center),
            const Divider(),

            // --- 4. LISTA DISPOSITIVI TROVATI ---
            Expanded(
              child: ListView.builder(
                itemCount: state.scanResults.length,
                itemBuilder: (context, index) {
                  final result = state.scanResults[index];
                  // Mostra solo dispositivi con un nome
                  if (result.device.advName.isEmpty) return const SizedBox.shrink();
                  
                  return ListTile(
                    leading: const Icon(Icons.bluetooth),
                    title: Text(result.device.advName),
                    subtitle: Text(result.device.remoteId.toString()),
                    trailing: ElevatedButton(
                      onPressed: () => context.read<AppState>().connectToDevice(
                        result.device,
                        serviceUuid: serviceUuid,
                        characteristicUuid: characteristicUuid,
                      ),
                      child: const Text("Connetti"),
                    ),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}