"""Inventory arithmetic for the ten extracted native Combat Honor rewards.

Profile map ``m`` uses numeric delta mode 2: updates contain new minus old.
Launch reset and UUID replay handling belong to the owning LocalProfile store.
"""
import json
from pathlib import Path
import re


_catalog = json.loads((Path(__file__).with_name('fixtures') / 'native-boosts.json').read_text(encoding='utf-8'))
BOOST_NAMESPACE = _catalog['profile_namespace']
BOOST_IDS = tuple(item['reward_id'] for item in _catalog['boosts'])
INITIAL_COUNT = _catalog['initial_count']
MAX_COUNT = 0x7fffffff
_TAGS = {f'{{{BOOST_NAMESPACE}}}{key}': key for key in BOOST_IDS}
if (BOOST_NAMESPACE != 'm' or type(INITIAL_COUNT) is not int or INITIAL_COUNT != 5
        or len(BOOST_IDS) != 10 or len(set(BOOST_IDS)) != 10
        or any(not re.fullmatch('[A-Z][A-Z0-9]{1,7}', key) for key in BOOST_IDS)):
    raise ValueError('Invalid extracted Boost catalog')


def launch_inventory():
    """Return a fresh launch allocation; callers decide when to persist it."""
    return dict.fromkeys(BOOST_IDS, INITIAL_COUNT)


def validate_inventory(inventory):
    if not isinstance(inventory, dict) or set(inventory) != set(BOOST_IDS):
        raise ValueError('Invalid Boost inventory keys')
    if any(type(count) is not int or not 0 <= count <= MAX_COUNT for count in inventory.values()):
        raise ValueError('Invalid Boost inventory count')
    return inventory


def parse_delta(element):
    """Read one known, flat native m:RewardID signed int32 update."""
    if element.tag not in _TAGS or element.attrib or len(element):
        raise ValueError('Unsupported Boost update field')
    if not isinstance(element.text, str) or not re.fullmatch(r'-?[0-9]{1,10}', element.text):
        raise ValueError('Invalid Boost delta integer')
    delta = int(element.text)
    if not -0x80000000 <= delta <= MAX_COUNT:
        raise ValueError('Boost delta out of range')
    return _TAGS[element.tag], delta


def apply_deltas(inventory, deltas):
    """Validate the complete operation before returning new counts."""
    validate_inventory(inventory)
    if not isinstance(deltas, dict) or set(deltas) - set(BOOST_IDS):
        raise ValueError('Unknown Boost delta keys')
    updated = dict(inventory)
    for key, delta in deltas.items():
        if type(delta) is not int or not -0x80000000 <= delta <= MAX_COUNT:
            raise ValueError('Invalid Boost delta')
        updated[key] += delta
    return validate_inventory(updated)
