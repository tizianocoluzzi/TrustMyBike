import pandas as pd
import numpy as np
import tensorflow as tf

# 1. Load the attached CSV
df = pd.read_csv('data.csv')
data = df['data 0'].values.astype(np.float32)

# 2. Normalization bounds (0 to 20 m/s^2 allows detecting huge +/- 1G bumps)
MIN_ACCEL = 0.0
MAX_ACCEL = 20.0

def normalize(val):
    return (val - MIN_ACCEL) / (MAX_ACCEL - MIN_ACCEL)

normalized_data = normalize(data).reshape(-1, 1)

# 3. Train the Tiny Autoencoder
model = tf.keras.Sequential([
    tf.keras.layers.Input(shape=(1,)),
    tf.keras.layers.Dense(8, activation='relu'),
    tf.keras.layers.Dense(1, activation='sigmoid')
])

model.compile(optimizer='adam', loss='mse')
model.fit(normalized_data, normalized_data, epochs=30, batch_size=32, verbose=1)

# 4. Convert to TensorFlow Lite
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

# 5. Generate the C++ Source File
with open("model_data.cpp", "w") as f:
    f.write('#include "ml/model_data.h"\n\n')
    f.write('const unsigned char autoencoder_model_data[] = {\n')
    hex_data = [f"0x{b:02x}" for b in tflite_model]
    for i in range(0, len(hex_data), 12):
        f.write("  " + ", ".join(hex_data[i:i+12]) + ",\n")
    f.write("};\n")
    f.write(f"const unsigned int autoencoder_model_data_len = {len(tflite_model)};\n")

print("Generated model_data.cpp successfully.")