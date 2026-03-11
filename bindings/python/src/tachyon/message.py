from dataclasses import dataclass


@dataclass(slots=True)
class Message:
    type_id: int
    size: int
    data: bytes | memoryview
