from sqlalchemy import Column, Integer, Float, String
from database import Base


class PathRecord(Base):
    __tablename__ = "paths"

    id = Column(Integer, primary_key=True, index=True)

    from_latitude = Column(Float, nullable=False)
    from_longitude = Column(Float, nullable=False)
    from_timestamp = Column(String, nullable=False)
    
    to_latitude = Column(Float, nullable=False)
    to_longitude = Column(Float, nullable=False)
    to_timestamp = Column(String, nullable=False)
    
    score = Column(Integer, nullable=False)
