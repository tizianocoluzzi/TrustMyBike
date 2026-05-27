import json
import os

import paho.mqtt.client as mqtt

from database import SessionLocal
from models import PathRecord

MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", 1883))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "trustmybike/rad_quality_batch")


def on_connect(client, userdata, flags, rc):
    print(f"Connected with result code {rc}")
    client.subscribe(MQTT_TOPIC)
    print(f"Subscribed to topic: {MQTT_TOPIC}")



def on_message(client, userdata, msg):
    print(f"[MQTT] Message received on topic: {msg.topic}")
    print(f"[MQTT] Raw payload: {msg.payload.decode()}")
    try:
        payload = json.loads(msg.payload.decode())

        # support both:
        # single object
        # array of objects
        if not isinstance(payload, list):
            payload = [payload]

        db = SessionLocal()

        for item in payload:

            required_fields = ["from", "to", "score"]

            for field in required_fields:
                if field not in item:
                    raise ValueError(f"Missing field: {field}")

            from_data = item["from"]
            to_data = item["to"]

            record = PathRecord(
                from_latitude=from_data["latitude"],
                from_longitude=from_data["longitude"],
                from_timestamp=from_data["timestamp"],

                to_latitude=to_data["latitude"],
                to_longitude=to_data["longitude"],
                to_timestamp=to_data["timestamp"],

                score=item["score"]
            )

            db.add(record)

        db.commit()

        print(f"Saved {len(payload)} records")

    except Exception as e:
        print(f"Error processing message: {e}")
client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message


def start_mqtt():
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start()
