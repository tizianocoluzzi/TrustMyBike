import re
import json
import itertools
from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf
from sklearn.metrics import cohen_kappa_score, confusion_matrix
from sklearn.model_selection import StratifiedGroupKFold
from sklearn.utils.class_weight import compute_class_weight


BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR.parent / "data"

MOTION_FEATURES = ["ax", "ay", "az", "gx", "gy", "gz"]
RAW_COLUMNS = MOTION_FEATURES + ["vel"]
VEL_FEATURES = ["vel_mean", "vel_std", "vel_last", "vel_zero_ratio"]

N_CLASSES = 5
SEED = 42

LABELS = {
    1: 4, 2: 3, 3: 4, 5: 5, 6: 5, 7: 4, 8: 3, 9: 3,
    10: 2, 12: 4, 13: 4, 14: 2, 18: 2, 19: 3, 20: 4,
    21: 3, 22: 2, 23: 3, 24: 2, 26: 2
}

SEARCH_SPACE = {
    "window": [64, 128, 192],
    "stride": [16, 32],
    "filters": [(12, 24, 24), (16, 24, 32), (24, 32, 48)],
    "kernels": [(5, 5, 3), (7, 5, 3)],
    "dense_units_motion": [16, 24],
    "dense_units_vel": [6, 8, 12],
    "dense_units_fusion": [12, 16, 24],
    "dropout_motion": [0.10, 0.20],
    "dropout_vel": [0.10],
    "dropout_fusion": [0.10, 0.20],
    "gaussian_noise": [0.0, 0.05],
    "l2_reg": [1e-4, 5e-4],
    "learning_rate": [1e-3, 5e-4],
    "use_class_weights": [False, True],
}

MAX_TRIALS = 10
CV_SPLITS = 2
CV_EPOCHS = 12
FINAL_EPOCHS = 40
BATCH_SIZE = 32
TENSORFLOW_VERBOSE = 0


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


def load_recordings():
    recordings = {}
    rec_labels = {}
    for csv_path in sorted(DATA_DIR.glob("data_*.csv")):
        rec_id = recording_id_from_name(csv_path.name)
        if rec_id not in LABELS:
            continue
        df = load_csv(csv_path)
        recordings[rec_id] = df
        rec_labels[rec_id] = LABELS[rec_id]
    return recordings, rec_labels


def build_windows_for_config(recordings, rec_labels, window, stride):
    x_motion, x_vel, y_all, g_all = [], [], [], []

    for rec_id, df in recordings.items():
        motion = df[MOTION_FEATURES].to_numpy(dtype=np.float32)
        vel = df["vel"].to_numpy(dtype=np.float32)
        label = rec_labels[rec_id] - 1

        for start in range(0, len(df) - window + 1, stride):
            end = start + window
            x_motion.append(motion[start:end])
            x_vel.append(velocity_window_features(vel[start:end]))
            y_all.append(label)
            g_all.append(rec_id)

    if not x_motion:
        raise RuntimeError(f"No windows created for window={window}, stride={stride}")

    return (
        np.asarray(x_motion, dtype=np.float32),
        np.asarray(x_vel, dtype=np.float32),
        np.asarray(y_all, dtype=np.int32),
        np.asarray(g_all, dtype=np.int32),
    )


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
        f"off_by_{i}": int(np.sum(abs_err == i)) for i in range(5)
    }

    per_class_accuracy = {}
    for cls in range(1, N_CLASSES + 1):
        mask = (y_true == cls)
        per_class_accuracy[str(cls)] = None if np.sum(mask) == 0 else float(np.mean(y_pred[mask] == y_true[mask]))

    return {
        "accuracy": accuracy,
        "expected_score_mae": mae_expected,
        "linear_weighted_kappa": linear_kappa,
        "quadratic_weighted_kappa": quadratic_kappa,
        "within_1_class": float(np.mean(abs_err <= 1)),
        "within_2_classes": float(np.mean(abs_err <= 2)),
        "mean_abs_class_error": float(np.mean(abs_err)),
        "max_abs_class_error": int(np.max(abs_err)),
        "error_histogram": error_hist,
        "confusion_matrix": cm.tolist(),
        "per_class_accuracy": per_class_accuracy,
    }


def build_model(
    window,
    filters,
    kernels,
    dense_units_motion,
    dense_units_vel,
    dense_units_fusion,
    dropout_motion,
    dropout_vel,
    dropout_fusion,
    gaussian_noise,
    l2_reg,
    learning_rate,
):
    reg = tf.keras.regularizers.l2(l2_reg)

    f1, f2, f3 = filters
    k1, k2, k3 = kernels

    motion_in = tf.keras.Input(shape=(window, len(MOTION_FEATURES)), name="motion")
    vel_in = tf.keras.Input(shape=(len(VEL_FEATURES),), name="vel_features")

    x = tf.keras.layers.Conv1D(f1, k1, padding="same", activation="relu", kernel_regularizer=reg)(motion_in)
    x = tf.keras.layers.MaxPooling1D(2)(x)
    x = tf.keras.layers.Conv1D(f2, k2, padding="same", activation="relu", kernel_regularizer=reg)(x)
    x = tf.keras.layers.MaxPooling1D(2)(x)
    x = tf.keras.layers.Conv1D(f3, k3, padding="same", activation="relu", kernel_regularizer=reg)(x)
    x = tf.keras.layers.GlobalAveragePooling1D()(x)
    x = tf.keras.layers.Dropout(dropout_motion)(x)
    x = tf.keras.layers.Dense(dense_units_motion, activation="relu", kernel_regularizer=reg)(x)

    if gaussian_noise > 0:
        v = tf.keras.layers.GaussianNoise(gaussian_noise)(vel_in)
    else:
        v = vel_in
    v = tf.keras.layers.Dense(dense_units_vel, activation="relu", kernel_regularizer=reg)(v)
    v = tf.keras.layers.Dropout(dropout_vel)(v)
    v = tf.keras.layers.Dense(max(4, dense_units_vel - 2), activation="relu", kernel_regularizer=reg)(v)

    z = tf.keras.layers.Concatenate()([x, v])
    z = tf.keras.layers.Dense(dense_units_fusion, activation="relu", kernel_regularizer=reg)(z)
    z = tf.keras.layers.Dropout(dropout_fusion)(z)
    out = tf.keras.layers.Dense(N_CLASSES, activation="softmax", name="road_class")(z)

    model = tf.keras.Model(inputs=[motion_in, vel_in], outputs=out)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate),
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


def get_class_weights(y_train):
    classes = np.unique(y_train)
    class_weights_raw = compute_class_weight(class_weight="balanced", classes=classes, y=y_train)
    return {int(c): float(w) for c, w in zip(classes, class_weights_raw)}


def make_trial_configs():
    keys = list(SEARCH_SPACE.keys())
    values = [SEARCH_SPACE[k] for k in keys]
    all_cfgs = [dict(zip(keys, combo)) for combo in itertools.product(*values)]
    rng = np.random.default_rng(SEED)
    rng.shuffle(all_cfgs)
    return all_cfgs[:MAX_TRIALS]


def pick_final_split(groups, y, rec_labels):
    rec_ids = np.array(sorted(rec_labels.keys()), dtype=np.int32)
    rec_y = np.array([rec_labels[r] - 1 for r in rec_ids], dtype=np.int32)
    n_splits = min(CV_SPLITS, len(rec_ids))
    if n_splits < 2:
        raise RuntimeError("Not enough recordings for grouped CV.")
    sgkf = StratifiedGroupKFold(n_splits=n_splits, shuffle=True, random_state=SEED)
    best_split = None
    best_score = -1
    for tr_r, va_r in sgkf.split(rec_ids, rec_y, groups=rec_ids):
        val_groups = rec_ids[va_r]
        val_mask = np.isin(groups, val_groups)
        y_val = y[val_mask]
        present = np.sum(np.bincount(y_val, minlength=N_CLASSES) > 0)
        if present > best_score:
            best_score = present
            best_split = (tr_r, va_r)
    tr_r, va_r = best_split
    train_groups = rec_ids[tr_r]
    val_groups = rec_ids[va_r]
    return np.isin(groups, train_groups), np.isin(groups, val_groups), train_groups, val_groups


def run_cv_for_config(config, recordings, rec_labels):
    tf.keras.backend.clear_session()

    x_motion_all, x_vel_all, y_all, g_all = build_windows_for_config(
        recordings, rec_labels, config["window"], config["stride"]
    )

    rec_ids = np.array(sorted(rec_labels.keys()), dtype=np.int32)
    rec_y = np.array([rec_labels[r] - 1 for r in rec_ids], dtype=np.int32)
    n_splits = min(CV_SPLITS, len(rec_ids))
    if n_splits < 2:
        raise RuntimeError("Not enough recordings for grouped CV.")

    sgkf = StratifiedGroupKFold(n_splits=n_splits, shuffle=True, random_state=SEED)
    fold_metrics = []

    for fold_idx, (tr_r, va_r) in enumerate(sgkf.split(rec_ids, rec_y, groups=rec_ids), start=1):
        train_groups = rec_ids[tr_r]
        val_groups = rec_ids[va_r]

        train_mask = np.isin(g_all, train_groups)
        val_mask = np.isin(g_all, val_groups)

        x_motion_train = x_motion_all[train_mask]
        x_vel_train = x_vel_all[train_mask]
        y_train = y_all[train_mask]

        x_motion_val = x_motion_all[val_mask]
        x_vel_val = x_vel_all[val_mask]
        y_val = y_all[val_mask]

        motion_mean = x_motion_train.mean(axis=(0, 1), dtype=np.float32)
        motion_std = x_motion_train.std(axis=(0, 1), dtype=np.float32) + 1e-6
        vel_mean = x_vel_train.mean(axis=0, dtype=np.float32)
        vel_std = x_vel_train.std(axis=0, dtype=np.float32) + 1e-6

        x_motion_train = (x_motion_train - motion_mean) / motion_std
        x_motion_val = (x_motion_val - motion_mean) / motion_std
        x_vel_train = (x_vel_train - vel_mean) / vel_std
        x_vel_val = (x_vel_val - vel_mean) / vel_std

        model = build_model(
            window=config["window"],
            filters=config["filters"],
            kernels=config["kernels"],
            dense_units_motion=config["dense_units_motion"],
            dense_units_vel=config["dense_units_vel"],
            dense_units_fusion=config["dense_units_fusion"],
            dropout_motion=config["dropout_motion"],
            dropout_vel=config["dropout_vel"],
            dropout_fusion=config["dropout_fusion"],
            gaussian_noise=config["gaussian_noise"],
            l2_reg=config["l2_reg"],
            learning_rate=config["learning_rate"],
        )

        callbacks = [
            tf.keras.callbacks.EarlyStopping(
                monitor="val_expected_score_metric",
                mode="min",
                patience=5,
                min_delta=1e-3,
                restore_best_weights=True,
                verbose=0,
            )
        ]

        class_weight = get_class_weights(y_train) if config["use_class_weights"] else None

        model.fit(
            x=[x_motion_train, x_vel_train],
            y=y_train,
            validation_data=([x_motion_val, x_vel_val], y_val),
            epochs=CV_EPOCHS,
            batch_size=BATCH_SIZE,
            class_weight=class_weight,
            callbacks=callbacks,
            verbose=TENSORFLOW_VERBOSE,
        )

        probs = model.predict([x_motion_val, x_vel_val], verbose=0)
        metrics = evaluate_ordinal_metrics(y_val, probs)
        metrics["fold"] = fold_idx
        fold_metrics.append(metrics)
        tf.keras.backend.clear_session()

    summary = {
        "cv_mean_accuracy": float(np.mean([m["accuracy"] for m in fold_metrics])),
        "cv_mean_expected_score_mae": float(np.mean([m["expected_score_mae"] for m in fold_metrics])),
        "cv_mean_linear_weighted_kappa": float(np.mean([m["linear_weighted_kappa"] for m in fold_metrics])),
        "cv_mean_quadratic_weighted_kappa": float(np.mean([m["quadratic_weighted_kappa"] for m in fold_metrics])),
        "cv_mean_within_1_class": float(np.mean([m["within_1_class"] for m in fold_metrics])),
        "cv_mean_within_2_classes": float(np.mean([m["within_2_classes"] for m in fold_metrics])),
        "cv_std_quadratic_weighted_kappa": float(np.std([m["quadratic_weighted_kappa"] for m in fold_metrics])),
        "folds": fold_metrics,
    }
    return summary


def retrain_best_model(best_config, recordings, rec_labels):
    x_motion_all, x_vel_all, y_all, g_all = build_windows_for_config(
        recordings, rec_labels, best_config["window"], best_config["stride"]
    )

    train_mask, val_mask, train_groups, val_groups = pick_final_split(g_all, y_all, rec_labels)

    x_motion_train = x_motion_all[train_mask]
    x_vel_train = x_vel_all[train_mask]
    y_train = y_all[train_mask]

    x_motion_val = x_motion_all[val_mask]
    x_vel_val = x_vel_all[val_mask]
    y_val = y_all[val_mask]

    print("Final train class counts:", {int(c + 1): int(np.sum(y_train == c)) for c in range(N_CLASSES)})
    print("Final val class counts:", {int(c + 1): int(np.sum(y_val == c)) for c in range(N_CLASSES)})
    print("Final val recording labels:", {int(r): int(rec_labels[int(r)]) for r in np.unique(g_all[val_mask])})

    motion_mean = x_motion_train.mean(axis=(0, 1), dtype=np.float32)
    motion_std = x_motion_train.std(axis=(0, 1), dtype=np.float32) + 1e-6
    vel_mean = x_vel_train.mean(axis=0, dtype=np.float32)
    vel_std = x_vel_train.std(axis=0, dtype=np.float32) + 1e-6

    x_motion_train_n = (x_motion_train - motion_mean) / motion_std
    x_motion_val_n = (x_motion_val - motion_mean) / motion_std
    x_vel_train_n = (x_vel_train - vel_mean) / vel_std
    x_vel_val_n = (x_vel_val - vel_mean) / vel_std

    model = build_model(
        window=best_config["window"],
        filters=tuple(best_config["filters"]),
        kernels=tuple(best_config["kernels"]),
        dense_units_motion=best_config["dense_units_motion"],
        dense_units_vel=best_config["dense_units_vel"],
        dense_units_fusion=best_config["dense_units_fusion"],
        dropout_motion=best_config["dropout_motion"],
        dropout_vel=best_config["dropout_vel"],
        dropout_fusion=best_config["dropout_fusion"],
        gaussian_noise=best_config["gaussian_noise"],
        l2_reg=best_config["l2_reg"],
        learning_rate=best_config["learning_rate"],
    )

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

    class_weight = get_class_weights(y_train) if best_config["use_class_weights"] else None

    history = model.fit(
        x=[x_motion_train_n, x_vel_train_n],
        y=y_train,
        validation_data=([x_motion_val_n, x_vel_val_n], y_val),
        epochs=FINAL_EPOCHS,
        batch_size=BATCH_SIZE,
        class_weight=class_weight,
        callbacks=callbacks,
        verbose=1,
    )

    val_probs = model.predict([x_motion_val_n, x_vel_val_n], verbose=0)
    metrics = evaluate_ordinal_metrics(y_val, val_probs)

    with open(BASE_DIR / "normalization.json", "w") as f:
        json.dump({
            "window": best_config["window"],
            "stride": best_config["stride"],
            "motion_features": MOTION_FEATURES,
            "velocity_features": VEL_FEATURES,
            "motion_mean": motion_mean.tolist(),
            "motion_std": motion_std.tolist(),
            "velocity_mean": vel_mean.tolist(),
            "velocity_std": vel_std.tolist(),
            "best_config": best_config,
        }, f, indent=2)

    with open(BASE_DIR / "training_summary.json", "w") as f:
        json.dump({
            "best_config": best_config,
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
            "history": {k: [float(vv) for vv in v] for k, v in history.history.items()},
            "val_groups": [int(x) for x in val_groups],
            "train_groups": [int(x) for x in train_groups],
        }, f, indent=2)

    tflite_model = export_tflite(model, x_motion_train_n, x_vel_train_n)

    tflite_path = BASE_DIR / "road_quality_int8.tflite"
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)

    header_path = BASE_DIR / "road_quality_model_data.h"
    write_header(tflite_model, header_path)

    model.save(BASE_DIR / "road_quality_fusion.keras")

    return metrics


def main():
    tf.keras.utils.set_random_seed(SEED)

    recordings, rec_labels = load_recordings()
    trial_configs = make_trial_configs()

    all_results = []
    best_result = None

    print(f"Running {len(trial_configs)} CV trials...")
    for idx, config in enumerate(trial_configs, start=1):
        print(f"\n=== Trial {idx}/{len(trial_configs)} ===")
        print("Config:", config)

        summary = run_cv_for_config(config, recordings, rec_labels)

        result = {
            "trial": idx,
            "config": {
                **config,
                "filters": list(config["filters"]),
                "kernels": list(config["kernels"]),
            },
            **summary,
        }
        all_results.append(result)

        print(
            "CV mean QWK={:.4f}, MAE={:.4f}, acc={:.4f}, within1={:.4f}".format(
                summary["cv_mean_quadratic_weighted_kappa"],
                summary["cv_mean_expected_score_mae"],
                summary["cv_mean_accuracy"],
                summary["cv_mean_within_1_class"],
            )
        )

        if best_result is None:
            best_result = result
        else:
            cur_key = (
                result["cv_mean_quadratic_weighted_kappa"],
                -result["cv_mean_expected_score_mae"],
                result["cv_mean_within_1_class"],
            )
            bst_key = (
                best_result["cv_mean_quadratic_weighted_kappa"],
                -best_result["cv_mean_expected_score_mae"],
                best_result["cv_mean_within_1_class"],
            )
            if cur_key > bst_key:
                best_result = result

    with open(BASE_DIR / "cv_results.json", "w") as f:
        json.dump(all_results, f, indent=2)

    rows = []
    for r in all_results:
        rows.append({
            "trial": r["trial"],
            "window": r["config"]["window"],
            "stride": r["config"]["stride"],
            "filters": str(tuple(r["config"]["filters"])),
            "kernels": str(tuple(r["config"]["kernels"])),
            "dense_units_motion": r["config"]["dense_units_motion"],
            "dense_units_vel": r["config"]["dense_units_vel"],
            "dense_units_fusion": r["config"]["dense_units_fusion"],
            "dropout_motion": r["config"]["dropout_motion"],
            "dropout_vel": r["config"]["dropout_vel"],
            "dropout_fusion": r["config"]["dropout_fusion"],
            "gaussian_noise": r["config"]["gaussian_noise"],
            "l2_reg": r["config"]["l2_reg"],
            "learning_rate": r["config"]["learning_rate"],
            "use_class_weights": r["config"]["use_class_weights"],
            "cv_mean_accuracy": r["cv_mean_accuracy"],
            "cv_mean_expected_score_mae": r["cv_mean_expected_score_mae"],
            "cv_mean_linear_weighted_kappa": r["cv_mean_linear_weighted_kappa"],
            "cv_mean_quadratic_weighted_kappa": r["cv_mean_quadratic_weighted_kappa"],
            "cv_mean_within_1_class": r["cv_mean_within_1_class"],
            "cv_mean_within_2_classes": r["cv_mean_within_2_classes"],
            "cv_std_quadratic_weighted_kappa": r["cv_std_quadratic_weighted_kappa"],
        })

    pd.DataFrame(rows).sort_values(
        ["cv_mean_quadratic_weighted_kappa", "cv_mean_expected_score_mae", "cv_mean_within_1_class"],
        ascending=[False, True, False],
    ).to_csv(BASE_DIR / "cv_results.csv", index=False)

    print("\nBest CV config:")
    print(json.dumps(best_result["config"], indent=2))
    print(
        "Best CV metrics: QWK={:.4f}, MAE={:.4f}, acc={:.4f}, within1={:.4f}".format(
            best_result["cv_mean_quadratic_weighted_kappa"],
            best_result["cv_mean_expected_score_mae"],
            best_result["cv_mean_accuracy"],
            best_result["cv_mean_within_1_class"],
        )
    )

    final_metrics = retrain_best_model(best_result["config"], recordings, rec_labels)

    print("\nFinal retrained model metrics:")
    print(f"Validation accuracy: {final_metrics['accuracy']:.4f}")
    print(f"Validation expected-score MAE: {final_metrics['expected_score_mae']:.4f}")
    print(f"Validation linear weighted kappa: {final_metrics['linear_weighted_kappa']:.4f}")
    print(f"Validation quadratic weighted kappa: {final_metrics['quadratic_weighted_kappa']:.4f}")
    print(f"Within 1 class: {final_metrics['within_1_class']:.4f}")
    print(f"Within 2 classes: {final_metrics['within_2_classes']:.4f}")
    print("Error histogram:", final_metrics["error_histogram"])
    print("Per-class accuracy:", final_metrics["per_class_accuracy"])
    print("Confusion matrix:")
    for row in final_metrics["confusion_matrix"]:
        print(row)


if __name__ == "__main__":
    main()