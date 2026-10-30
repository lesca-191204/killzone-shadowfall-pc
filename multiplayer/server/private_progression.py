"""Installed-game progression floors for private multiplayer profiles."""
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

FIXTURES = Path(__file__).with_name('fixtures')
_CHALLENGES = json.loads((FIXTURES / 'private-challenges.json').read_text(encoding='utf-8'))
CHALLENGE_FLOORS = _CHALLENGES['counter_floors']
DAILY_FLOORS = {key: CHALLENGE_FLOORS[key] for key in _CHALLENGES['daily_counter_keys']}
MAXIMUM_CHALLENGE_COUNT = json.loads((FIXTURES / 'private-progression.json').read_text(
    encoding='utf-8'))['maximum_challenge_count']


def _daily_progression(data):
    fields = data.findall('serializedDailyChallengeMap')
    if len(fields) > 1:
        raise ValueError('Duplicate daily challenge map')
    field = fields[0] if fields else None
    entries = {}
    if field is not None:
        if field.attrib or len(field):
            raise ValueError('Invalid daily challenge map')
        text = field.text or ''
        if len(text) > 65536:
            raise ValueError('Daily challenge map is too large')
        for entry in text.removesuffix(',').split(',') if text else ():
            match = re.fullmatch(r'([A-Za-z][A-Za-z0-9_.]{0,63})=([0-9]{1,10})\|([01])', entry)
            if match is None:
                raise ValueError('Invalid daily challenge entry')
            key, progress, claimed = match.groups()
            if key in entries or int(progress) > 0x7fffffff:
                raise ValueError('Duplicate or out-of-range daily challenge entry')
            entries[key] = (int(progress), claimed)
    for key, floor in DAILY_FLOORS.items():
        progress, claimed = entries.get(key, (0, '0'))
        entries[key] = (max(progress, floor), claimed)
    result = ''.join(f'{key}={progress}|{claimed},'
                     for key, (progress, claimed) in entries.items())
    if field is None:
        field = ET.SubElement(data, 'serializedDailyChallengeMap')
    field.text = result


def apply_private_progression(data):
    """Raise native counters to extracted thresholds, preserving higher values.

    Validate all affected existing fields before changing anything. The separate
    daily rotation timestamps and achievement accolades retain native values.
    """
    if data.get('type') == 'DailyChallengesProfile':
        _daily_progression(data)
        return
    if data.get('type') == 'profile':
        floors = {'{c}Total': MAXIMUM_CHALLENGE_COUNT}
    elif data.get('type') == 'challengesProfile':
        floors = {'{ch}' + key: value for key, value in CHALLENGE_FLOORS.items()}
    else:
        return
    current = {}
    for child in data:
        if child.tag not in floors:
            continue
        if child.tag in current:
            raise ValueError('Duplicate private progression counter')
        if child.attrib or len(child) or not re.fullmatch(r'[0-9]{1,10}', child.text or ''):
            raise ValueError('Invalid private progression counter')
        value = int(child.text)
        if value > 0x7fffffff:
            raise ValueError('Private progression counter out of range')
        current[child.tag] = child, value
    for tag, floor in floors.items():
        if tag in current:
            child, value = current[tag]
            if value < floor:
                child.text = str(floor)
        else:
            ET.SubElement(data, tag).text = str(floor)
