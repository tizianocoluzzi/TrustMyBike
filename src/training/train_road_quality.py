import re
import json
from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf
from sklearn.model_selection import train_test_split
from sklearn.utils.class_weight import compute_class_weight

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR.parent / "data"

FEATURES = ["ax", "ay", "az", "gx", "gy", "gz", "temp", "vel"]
WINDOW = 64
STRIDE = 16
N_CLASSES = 5

# Clean labels only for v1
LABELS = {
    1: 4, 2: 3, 3: 4, 5: 5, 6: 5, 7: 4, 8: 3, 9: 3,
    10: 2, 12: 4, 13: 4, 14: 2, 18: 2, 19: 3, 20: 4,
    21: 3, 22: 2, 23: 3, 24: 2, 26: 2
}

def recording_id_from_name(name: str) -> int:
    m = re.match(r"data_(\d+)(?:-[^.]*)?\.csv$", name)
    if not m:
        raise ValueError(f"Filename not supported: {name}")
    return int(m.group(1))

def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    df = df[["ax", "ay", "az", "gx", "gy", "gz", "temp", "vel"]].copy()
    df = df.apply(pd.to_numeric, errors="coerce")
    df["vel"] = df["vel"].clip(0, 40)
    df = df.replace([np.inf, -np.inf], np.nan).dropna()
    return df

def build_windows(values: np.ndarray, label: int, rec_id: int):
    X, y, groups = [], [], []
    for start in range(0, len(values) - WINDOW + 1, STRIDE):
        X.append(values[start:start + WINDOW])
        y.append(label - 1)
        groups.append(rec_id)
    return X, y, groups

def main():
    X_all, y_all, g_all = [], [], []

    for csv_path in sorted(DATA_DIR.glob("data_*.csv")):
        rec_id = recording_id_from_name(csv_path.name)
        if rec_id not in LABELS:
            continue

        df = load_csv(csv_path)
        X, y, g = build_windows(df.values.astype(np.float32), LABELS[rec_id], rec_id)

        X_all.extend(X)
        y_all.extend(y)
        g_all.extend(g)

    X_all = np.array(X_all, dtype=np.float32)
    y_all = np.array(y_all, dtype=np.int32)
    g_all = np.array(g_all, dtype=np.int32)

    unique_groups = np.unique(g_all)
    train_groups, val_groups = train_test_split(unique_groups, test_size=0.25, random_state=42)

    train_mask = np.isin(g_all, train_groups)
    val_mask = np.isin(g_all, val_groups)

    X_train, y_train = X_all[train_mask], y_all[train_mask]
    X_val, y_val = X_all[val_mask], y_all[val_mask]

    mean = X_train.mean(axis=(0, 1))
    std = X_train.std(axis=(0, 1)) + 1e-6

    X_train = (X_train - mean) / std
    X_val = (X_val - mean) / std

    classes = np.unique(y_train)
    cw = compute_class_weight(class_weight="balanced", classes=classes, y=y_train)
    class_weights = {int(c): float(w) for c, w in zip(classes, cw)}

    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(WINDOW, len(FEATURES))),
        tf.keras.layers.Conv1D(16, 5, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling1D(2),
        tf.keras.layers.Conv1D(32, 5, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling1D(2),
        tf.keras.layers.Conv1D(32, 3, padding="same", activation="relu"),
        tf.keras.layers.GlobalAveragePooling1D(),
        tf.keras.layers.Dense(24, activation="relu"),
        tf.keras.layers.Dense(N_CLASSES, activation="softmax"),
    ])

    model.compile(
        optimizer=tf.keras.optimizers.Adam(1e-3),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"]
    )

    model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=30,
        batch_size=32,
        class_weight=class_weights,
        verbose=1
    )

    with open(BASE_DIR / "normalization.json", "w") as f:
        json.dump({
            "features": FEATURES,
            "window": WINDOW,
            "stride": STRIDE,
            "mean": mean.tolist(),
            "std": std.tolist()
        }, f, indent=2)

    def representative_dataset():
        for i in range(min(200, len(X_train))):
            yield [X_train[i:i+1].astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8

    tflite_model = converter.convert()

    tflite_path = BASE_DIR / "road_quality_int8.tflite"
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)

    header_path = BASE_DIR / "road_quality_model_data.h"
    with open(header_path, "w") as f:
        f.write("#pragma once\n")
        f.write("const unsigned char road_quality_model_data[] = {\n")
        f.write(",".join(f"0x{b:02x}" for b in tflite_model))
        f.write("\n};\n")
        f.write(f"const unsigned int road_quality_model_data_len = {len(tflite_model)};\n")

    print("Training complete.")
    print(f"Saved: {tflite_path}")
    print(f"Saved: {header_path}")
    print(f"Saved: {BASE_DIR / 'normalization.json'}")

if __name__ == "__main__":
    main()