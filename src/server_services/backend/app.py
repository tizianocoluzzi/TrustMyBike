from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy.orm import Session

from database import Base, SessionLocal, engine
from models import PathRecord
from mqtt_client import start_mqtt

Base.metadata.create_all(bind=engine)

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.on_event("startup")
def startup_event():
    start_mqtt()


@app.get("/paths")
def get_paths():
    db: Session = SessionLocal()

    records = db.query(PathRecord).all()

    return [
        {
        "id": r.id,
        "from": {
            "latitude": r.from_latitude,
            "longitude": r.from_longitude,
            "timestamp": r.from_timestamp
        },
        "to": {
            "latitude": r.to_latitude,
            "longitude": r.to_longitude,
            "timestamp": r.to_timestamp
        },
        "score": r.score
    }
        for r in records
    ]
