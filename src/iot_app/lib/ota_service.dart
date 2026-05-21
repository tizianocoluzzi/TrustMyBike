import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'dart:convert';

// ── Constants ────────────────────────────────────────────────────────
const _repoBase =
    'https://github.com/tizianocoluzzi/TrustMyBike/releases/latest/download';

const _otaServiceUuid  = '12345678-1234-1234-1234-123456789abc';
const _otaCtrlUuid     = '12345678-1234-1234-1234-123456789ac0';
const _otaDataUuid     = '12345678-1234-1234-1234-123456789ac1';

const _chunkSize       = 128;
const _prefVersionKey  = 'fw_version';

// ── Data model ───────────────────────────────────────────────────────
class FirmwareVersion {
  final int    version;
  final String sha;
  final String file;

  FirmwareVersion({
    required this.version,
    required this.sha,
    required this.file,
  });

  factory FirmwareVersion.fromJson(Map<String, dynamic> j) =>
      FirmwareVersion(
        version: int.parse(j['version'].toString()),
        sha:     j['sha'],
        file:    j['file'],
      );
}

// ── Service ──────────────────────────────────────────────────────────
class OtaService {
  // progress 0.0–1.0, null when idle
  final ValueNotifier<double?> progress = ValueNotifier(null);
  final ValueNotifier<String>  status   = ValueNotifier('');

  // ── Version helpers ──────────────────────────────────────────────
  Future<int> _storedVersion() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getInt(_prefVersionKey) ?? 0;
  }

  Future<void> _saveVersion(int v) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt(_prefVersionKey, v);
  }

  // ── 1. Check GitHub for a newer version ─────────────────────────
  Future<FirmwareVersion?> checkForUpdate() async {
    try {
      final res = await http
          .get(Uri.parse('$_repoBase/version.json'))
          .timeout(const Duration(seconds: 10));

      if (res.statusCode != 200) {
        status.value = 'Version check failed (${res.statusCode})';
        return null;
      }

      final remote   = FirmwareVersion.fromJson(jsonDecode(res.body));
      final local    = await _storedVersion();

      if (remote.version > local) return remote;

      status.value = 'Firmware già aggiornato (v$local)';
      return null;
    } catch (e) {
      status.value = 'Errore check: $e';
      return null;
    }
  }

  // ── 2. Download .bin to temp dir (cached by SHA) ─────────────────
  Future<File?> downloadFirmware(FirmwareVersion ver) async {
    try {
      final dir  = await getTemporaryDirectory();
      final file = File('${dir.path}/${ver.file}');

      if (await file.exists()) {
        status.value = 'Firmware già in cache';
        return file;
      }

      status.value = 'Download firmware...';
      final res = await http
          .get(Uri.parse('$_repoBase/${ver.file}'))
          .timeout(const Duration(minutes: 5));

      if (res.statusCode != 200) {
        status.value = 'Download fallito (${res.statusCode})';
        return null;
      }

      await file.writeAsBytes(res.bodyBytes);
      status.value = 'Download completato';
      return file;
    } catch (e) {
      status.value = 'Errore download: $e';
      return null;
    }
  }

  // ── 3. Flash over BLE ────────────────────────────────────────────
  Future<bool> flashDevice(BluetoothDevice device, File firmware,
      FirmwareVersion ver,List<BluetoothService> services,) async {
    try {
      progress.value = 0.0;
      status.value   = 'Negoziazione MTU...';

      // Larger MTU = faster transfer
      await device.requestMtu(256);

      status.value = 'Ricerca caratteristiche OTA...';
      //final services = await device.discoverServices();

      BluetoothService? svc;
      for (final s in services) {
        if (s.uuid.toString().toLowerCase() == _otaServiceUuid) {
          svc = s;
          break;
        }
      }
      if (svc == null) {
        status.value = 'Servizio OTA non trovato';
        return false;
      }

      final ctrl = _findChar(svc, _otaCtrlUuid);
      final data = _findChar(svc, _otaDataUuid);

      if (ctrl == null || data == null) {
        status.value = 'Caratteristiche OTA mancanti';
        return false;
      }

      final bytes = await firmware.readAsBytes();
      final total = bytes.length;

      // START
      status.value = 'Avvio OTA ($total bytes)...';
      await ctrl.write(utf8.encode('START:$total'), withoutResponse: false);
      await Future.delayed(const Duration(milliseconds: 300));

      // Send chunks
      int offset = 0;
      while (offset < total) {
        final end   = (offset + _chunkSize).clamp(0, total);
        final chunk = bytes.sublist(offset, end);

        await data.write(chunk, withoutResponse: true);
        offset         = end;
        progress.value = offset / total;
        status.value   =
            'Flash: ${(offset / total * 100).toStringAsFixed(1)}%'
            ' ($offset / $total)';

        // Yield every 20 chunks to keep BLE stack happy
        //if (offset % (_chunkSize * 10) == 0) {
        await Future.delayed(const Duration(milliseconds: 20));
        //}
      }

      // END
      status.value = 'Verifica firmware...';
      await Future.delayed(const Duration(milliseconds: 300));
      await ctrl.write(utf8.encode('END'), withoutResponse: false);

      // Save version locally — device will reboot now
      await _saveVersion(ver.version);
      progress.value = 1.0;
      status.value   = 'OTA completato! Dispositivo in riavvio...';
      return true;
    } catch (e) {
      status.value   = 'Errore flash: $e';
      progress.value = null;
      return false;
    }
  }

  // ── Convenience: full pipeline in one call ───────────────────────
  Future<void> runOtaUpdate(BluetoothDevice? device) async {
    if (device == null) {
      status.value = 'Nessun dispositivo connesso';
      return;
    }

    final ver = await checkForUpdate();
    if (ver == null) return;

    status.value = 'Nuova versione trovata: v${ver.version}';
    final file = await downloadFirmware(ver);
    if (file == null) return;
    status.value = 'Scoperta servizi BLE...';
    final services = await device.discoverServices(); // ← single discovery here
    await flashDevice(device, file, ver, services);
  }

  // ── Helper ───────────────────────────────────────────────────────
  BluetoothCharacteristic? _findChar(BluetoothService svc, String uuid) =>
      svc.characteristics.cast<BluetoothCharacteristic?>().firstWhere(
            (c) => c!.uuid.toString().toLowerCase() == uuid,
            orElse: () => null,
          );

  void dispose() {
    progress.dispose();
    status.dispose();
  }
}