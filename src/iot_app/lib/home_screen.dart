import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'app_state.dart';

class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context) {
    // Collega questa schermata al "Cervello" (AppState)
    final state = context.watch<AppState>();

    return Scaffold(
      appBar: AppBar(
        title: const Text("ESP32 IoT Dashboard"),
        backgroundColor: Colors.blueAccent,
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // --- CARD DEL GPS ---
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
            
            const SizedBox(height: 20),

            // --- SEZIONE BLUETOOTH ---
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

            // --- LISTA DISPOSITIVI TROVATI ---
            Expanded(
              child: ListView.builder(
                itemCount: state.scanResults.length,
                itemBuilder: (context, index) {
                  final result = state.scanResults[index];
                  // Mostra solo dispositivi con un nome (per filtrare la spazzatura BLE)
                  if (result.device.advName.isEmpty) return const SizedBox.shrink();
                  
                  return ListTile(
                    leading: const Icon(Icons.bluetooth),
                    title: Text(result.device.advName),
                    subtitle: Text(result.device.remoteId.toString()),
                    trailing: ElevatedButton(
                      onPressed: () => context.read<AppState>().connectToDevice(result.device),
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