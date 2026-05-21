import re
import json
from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf
from sklearn.metrics import cohen_kappa_score, confusion_matrix
from sklearn.model_selection import GroupShuffleSplit
from sklearn.utils.class_weight import compute_class_weight

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR.parent / "data"

MOTION_FEATURES = ["ax", "ay", "az", "gx", "gy", "gz"]
RAW_COLUMNS = MOTION_FEATURES + ["vel"]
WINDOW = 64
STRIDE = 16
N_CLASSES = 5
VEL_FEATURES = ["vel_mean", "vel_std", "vel_last", "vel_zero_ratio"]
SEED = 42

LABELS = {
    1: 4, 2: 3, 3: 4, 5: 5, 6: 5, 7: 4, 8: 3, 9: 3,
    10: 2, 12: 4, 13: 4, 14: 2, 18: 2, 19: 3, 20: 4,
    21: 3, 22: 2, 23: 3, 24: 2, 26: 2
}


def recording_id_from_name(name: str) -> int:
    m = re.match(r"data_(\d+)(?:-[^.]+)?\.csv$", name)
    if not m:
        raise ValueError(f"Filename not supported: {name}")
    return int(m.group(1))


def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    df = df[RAW_COLUMNS].copy()
    df = df.apply(pd.to_numeric, errors="coerce")
    df["vel"] = df["vel"].clip(lower=0.0, upper=40.0)
    df = df.replace([np.inf, -np.inf], np.nan).dropna()
    return df


def velocity_window_features(vel_window: np.ndarray) -> np.ndarray:
    vel = np.asarray(vel_window, dtype=np.float32)
    zero_ratio = np.mean(vel <= 1e-3, dtype=np.float32)
    return np.array([
        np.mean(vel, dtype=np.float32),
        np.std(vel, dtype=np.float32),
        vel[-1],
        zero_ratio,
    ], dtype=np.float32)


def build_windows(df: pd.DataFrame, label: int, rec_id: int):
    x_motion, x_vel, y, groups = [], [], [], []
    motion = df[MOTION_FEATURES].values.astype(np.float32)
    vel = df["vel"].values.astype(np.float32)

    for start in range(0, len(df) - WINDOW + 1, STRIDE):
        end = start + WINDOW
        x_motion.append(motion[start:end])
        x_vel.append(velocity_window_features(vel[start:end]))
        y.append(label - 1)
        groups.append(rec_id)

    return x_motion, x_vel, y, groups


def expected_score_metric(y_true, y_pred):
    y_true = tf.cast(y_true, tf.float32) + 1.0
    class_ids = tf.cast(tf.range(1, N_CLASSES + 1), tf.float32)
    expected = tf.reduce_sum(y_pred * class_ids[tf.newaxis, :], axis=-1)
    return tf.reduce_mean(tf.abs(expected - y_true))


def evaluate_ordinal_metrics(y_true_zero_based: np.ndarray, probs: np.ndarray):
    y_true = y_true_zero_based.astype(np.int32) + 1
    y_pred = np.argmax(probs, axis=1).astype(np.int32) + 1

    expected_scores = (probs * np.arange(1, N_CLASSES + 1, dtype=np.float32)).sum(axis=1)
    mae_expected = float(np.mean(np.abs(expected_scores - y_true)))
    accuracy = float(np.mean(y_pred == y_true))

    linear_kappa = float(cohen_kappa_score(y_true, y_pred, weights="linear"))
    quadratic_kappa = float(cohen_kappa_score(y_true, y_pred, weights="quadratic"))

    cm = confusion_matrix(y_true, y_pred, labels=np.arange(1, N_CLASSES + 1))
    abs_err = np.abs(y_pred - y_true)

    error_hist = {
        "off_by_0": int(np.sum(abs_err == 0)),
        "off_by_1": int(np.sum(abs_err == 1)),
        "off_by_2": int(np.sum(abs_err == 2)),
        "off_by_3": int(np.sum(abs_err == 3)),
        "off_by_4": int(np.sum(abs_err == 4)),
    }

    per_class_accuracy = {}
    for cls in range(1, N_CLASSES + 1):
        mask = (y_true == cls)
        if np.sum(mask) == 0:
            per_class_accuracy[str(cls)] = None
        else:
            per_class_accuracy[str(cls)] = float(np.mean(y_pred[mask] == y_true[mask]))

    within_1 = float(np.mean(abs_err <= 1))
    within_2 = float(np.mean(abs_err <= 2))
    mean_abs_class_error = float(np.mean(abs_err))
    max_abs_class_error = int(np.max(abs_err))

    return {
        "accuracy": accuracy,
        "expected_score_mae": mae_expected,
        "linear_weighted_kappa": linear_kappa,
        "quadratic_weighted_kappa": quadratic_kappa,
        "within_1_class": within_1,
        "within_2_classes": within_2,
        "mean_abs_class_error": mean_abs_class_error,
        "max_abs_class_error": max_abs_class_error,
        "error_histogram": error_hist,
        "confusion_matrix": cm.tolist(),
        "per_class_accuracy": per_class_accuracy,
    }


def build_model():
    reg = tf.keras.regularizers.l2(1e-4)

    motion_in = tf.keras.Input(shape=(WINDOW, len(MOTION_FEATURES)), name="motion")
    vel_in = tf.keras.Input(shape=(len(VEL_FEATURES),), name="vel_features")

    x = tf.keras.layers.Conv1D(12, 5, padding="same", activation="relu", kernel_regularizer=reg)(motion_in)
    x = tf.keras.layers.MaxPooling1D(2)(x)
    x = tf.keras.layers.Conv1D(24, 5, padding="same", activation="relu", kernel_regularizer=reg)(x)
    x = tf.keras.layers.MaxPooling1D(2)(x)
    x = tf.keras.layers.Conv1D(24, 3, padding="same", activation="relu", kernel_regularizer=reg)(x)
    x = tf.keras.layers.GlobalAveragePooling1D()(x)
    x = tf.keras.layers.Dropout(0.20)(x)
    x = tf.keras.layers.Dense(16, activation="relu", kernel_regularizer=reg)(x)

    v = tf.keras.layers.GaussianNoise(0.05)(vel_in)
    v = tf.keras.layers.Dense(8, activation="relu", kernel_regularizer=reg)(v)
    v = tf.keras.layers.Dropout(0.10)(v)
    v = tf.keras.layers.Dense(6, activation="relu", kernel_regularizer=reg)(v)

    z = tf.keras.layers.Concatenate()([x, v])
    z = tf.keras.layers.Dense(12, activation="relu", kernel_regularizer=reg)(z)
    z = tf.keras.layers.Dropout(0.20)(z)
    out = tf.keras.layers.Dense(N_CLASSES, activation="softmax", name="road_class")(z)

    model = tf.keras.Model(inputs=[motion_in, vel_in], outputs=out)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(1e-3),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(),
        metrics=["accuracy", expected_score_metric],
    )
    return model


def representative_dataset(x_motion, x_vel):
    count = min(200, len(x_motion))
    for i in range(count):
        yield [
            x_motion[i:i + 1].astype(np.float32),
            x_vel[i:i + 1].astype(np.float32),
        ]


def export_tflite(model, x_motion_train, x_vel_train):
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: representative_dataset(x_motion_train, x_vel_train)
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    return converter.convert()


def write_header(tflite_model: bytes, header_path: Path):
    with open(header_path, "w") as f:
        f.write("#pragma once\n")
        f.write("const unsigned char road_quality_model_data[] = {\n")
        f.write(",".join(f"0x{b:02x}" for b in tflite_model))
        f.write("\n};\n")
        f.write(f"const unsigned int road_quality_model_data_len = {len(tflite_model)};\n")


def main():
    tf.keras.utils.set_random_seed(SEED)

    x_motion_all, x_vel_all, y_all, g_all = [], [], [], []

    for csv_path in sorted(DATA_DIR.glob("data_*.csv")):
        rec_id = recording_id_from_name(csv_path.name)
        if rec_id not in LABELS:
            continue

        df = load_csv(csv_path)
        x_m, x_v, y, g = build_windows(df, LABELS[rec_id], rec_id)
        x_motion_all.extend(x_m)
        x_vel_all.extend(x_v)
        y_all.extend(y)
        g_all.extend(g)

    x_motion_all = np.asarray(x_motion_all, dtype=np.float32)
    x_vel_all = np.asarray(x_vel_all, dtype=np.float32)
    y_all = np.asarray(y_all, dtype=np.int32)
    g_all = np.asarray(g_all, dtype=np.int32)

    if len(x_motion_all) == 0:
        raise RuntimeError("No windows were created. Check DATA_DIR and LABELS.")

    splitter = GroupShuffleSplit(n_splits=1, test_size=0.25, random_state=SEED)
    train_idx, val_idx = next(splitter.split(x_motion_all, y_all, groups=g_all))

    x_motion_train = x_motion_all[train_idx]
    x_motion_val = x_motion_all[val_idx]
    x_vel_train = x_vel_all[train_idx]
    x_vel_val = x_vel_all[val_idx]
    y_train = y_all[train_idx]
    y_val = y_all[val_idx]

    motion_mean = x_motion_train.mean(axis=(0, 1), dtype=np.float32)
    motion_std = x_motion_train.std(axis=(0, 1), dtype=np.float32) + 1e-6
    vel_mean = x_vel_train.mean(axis=0, dtype=np.float32)
    vel_std = x_vel_train.std(axis=0, dtype=np.float32) + 1e-6

    x_motion_train = (x_motion_train - motion_mean) / motion_std
    x_motion_val = (x_motion_val - motion_mean) / motion_std
    x_vel_train = (x_vel_train - vel_mean) / vel_std
    x_vel_val = (x_vel_val - vel_mean) / vel_std

    classes = np.unique(y_train)
    class_weights_raw = compute_class_weight(class_weight="balanced", classes=classes, y=y_train)
    class_weights = {int(c): float(w) for c, w in zip(classes, class_weights_raw)}

    model = build_model()

    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_expected_score_metric",
            mode="min",
            patience=8,
            min_delta=1e-3,
            restore_best_weights=True,
            verbose=1,
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_expected_score_metric",
            mode="min",
            factor=0.5,
            patience=4,
            min_lr=1e-5,
            verbose=1,
        ),
    ]

    history = model.fit(
        x=[x_motion_train, x_vel_train],
        y=y_train,
        validation_data=([x_motion_val, x_vel_val], y_val),
        epochs=40,
        batch_size=32,
        class_weight=class_weights,
        callbacks=callbacks,
        verbose=1,
    )

    val_probs = model.predict([x_motion_val, x_vel_val], verbose=0)
    metrics = evaluate_ordinal_metrics(y_val, val_probs)

    with open(BASE_DIR / "normalization.json", "w") as f:
        json.dump({
            "window": WINDOW,
            "stride": STRIDE,
            "motion_features": MOTION_FEATURES,
            "velocity_features": VEL_FEATURES,
            "motion_mean": motion_mean.tolist(),
            "motion_std": motion_std.tolist(),
            "velocity_mean": vel_mean.tolist(),
            "velocity_std": vel_std.tolist(),
        }, f, indent=2)

    with open(BASE_DIR / "training_summary.json", "w") as f:
        json.dump({
            "val_accuracy": metrics["accuracy"],
            "val_expected_score_mae": metrics["expected_score_mae"],
            "val_linear_weighted_kappa": metrics["linear_weighted_kappa"],
            "val_quadratic_weighted_kappa": metrics["quadratic_weighted_kappa"],
            "val_within_1_class": metrics["within_1_class"],
            "val_within_2_classes": metrics["within_2_classes"],
            "val_mean_abs_class_error": metrics["mean_abs_class_error"],
            "val_max_abs_class_error": metrics["max_abs_class_error"],
            "val_error_histogram": metrics["error_histogram"],
            "val_confusion_matrix": metrics["confusion_matrix"],
            "val_per_class_accuracy": metrics["per_class_accuracy"],
            "train_windows": int(len(x_motion_train)),
            "val_windows": int(len(x_motion_val)),
            "class_weights": class_weights,
            "history": {k: [float(vv) for vv in v] for k, v in history.history.items()},
        }, f, indent=2)

    tflite_model = export_tflite(model, x_motion_train, x_vel_train)

    tflite_path = BASE_DIR / "road_quality_int8.tflite"
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)

    header_path = BASE_DIR / "road_quality_model_data.h"
    write_header(tflite_model, header_path)

    model.save(BASE_DIR / "road_quality_fusion.keras")

    print("Training complete.")
    print(f"Saved: {BASE_DIR / 'road_quality_fusion.keras'}")
    print(f"Saved: {tflite_path}")
    print(f"Saved: {header_path}")
    print(f"Saved: {BASE_DIR / 'normalization.json'}")
    print(f"Saved: {BASE_DIR / 'training_summary.json'}")
    print(f"Validation accuracy: {metrics['accuracy']:.4f}")
    print(f"Validation expected-score MAE: {metrics['expected_score_mae']:.4f}")
    print(f"Validation linear weighted kappa: {metrics['linear_weighted_kappa']:.4f}")
    print(f"Validation quadratic weighted kappa: {metrics['quadratic_weighted_kappa']:.4f}")
    print(f"Within 1 class: {metrics['within_1_class']:.4f}")
    print(f"Within 2 classes: {metrics['within_2_classes']:.4f}")
    print("Error histogram:", metrics["error_histogram"])
    print("Per-class accuracy:", metrics["per_class_accuracy"])
    print("Confusion matrix:")
    for row in metrics["confusion_matrix"]:
        print(row)


if __name__ == "__main__":
    main()