import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'app_state.dart';
import 'home_screen.dart';

void main() {
  runApp(
    // Inizializziamo il gestore della logica (AppState) per tutta l'app
    ChangeNotifierProvider(
      create: (context) => AppState(),
      child: const MyIoTApp(),
    ),
  );
}

class MyIoTApp extends StatelessWidget {
  const MyIoTApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'IoT ESP32 Tracker',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: const HomeScreen(), // Carica la schermata principale
    );
  }
}