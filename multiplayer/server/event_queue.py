"""Durable, identity-addressed native events with retry after failed writes."""
from copy import deepcopy
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
from threading import Lock
from uuid import uuid4
import xml.etree.ElementTree as ET
from xml_storage import atomic_write


@dataclass(frozen=True)
class Delivery:
    identity: str
    key: str
    token: str
    payload: bytes


class EventQueue:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = Lock()
        self.inflight = {}
        self.connections = set()

    def _filename(self, identity):
        return self.directory / (sha256(identity.encode('utf-8')).hexdigest() + '.xml')

    def _read(self, identity):
        path = self._filename(identity)
        if not path.exists():
            return ET.Element('eventQueue', {'identity': identity})
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as error:
            raise OSError('Invalid stored event queue') from error
        if root.tag != 'eventQueue' or root.get('identity') != identity:
            raise OSError('Invalid stored event identity')
        return root

    def enqueue_once(self, identity, key, payload):
        event = ET.fromstring(payload)
        if event.tag != 'kinAssignmentMessage' or not key:
            raise ValueError('Expected a keyed native assignment')
        with self.lock:
            state = self._read(identity)
            if any(x.get('key') == key for x in state):
                return False
            pending = ET.SubElement(state, 'pending', {'key': key})
            pending.append(deepcopy(event))
            atomic_write(self._filename(identity), state)
            return True

    def claim(self, identity):
        with self.lock:
            if identity in self.inflight:
                return None
            state = self._read(identity)
            pending = state.find('pending')
            if pending is None:
                return None
            if len(pending) != 1 or not pending.get('key'):
                raise OSError('Invalid queued event')
            delivery = Delivery(identity, pending.get('key'), str(uuid4()),
                                ET.tostring(pending[0], encoding='utf-8'))
            self.inflight[identity] = delivery.token
            return delivery

    def acknowledge(self, delivery):
        with self.lock:
            if self.inflight.get(delivery.identity) != delivery.token:
                raise ValueError('Event delivery is not owned by this request')
            state = self._read(delivery.identity)
            pending = next((x for x in state.findall('pending') if x.get('key') == delivery.key), None)
            if pending is None:
                raise OSError('Claimed event disappeared')
            state.remove(pending)
            ET.SubElement(state, 'delivered', {'key': delivery.key})
            atomic_write(self._filename(delivery.identity), state)
            self.inflight.pop(delivery.identity, None)

    def release(self, delivery):
        with self.lock:
            if self.inflight.get(delivery.identity) == delivery.token:
                self.inflight.pop(delivery.identity, None)

    def connected(self, session):
        with self.lock:
            return session in self.connections

    def mark_connected(self, session):
        with self.lock:
            self.connections.add(session)
