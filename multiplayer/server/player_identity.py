"""Shared nickname contract for the PS4 OnlineID's 16-byte name field."""
import re

MAX_PLAYERS = 24
PLAYER_IDS = tuple(f'Player{i}' for i in range(1, MAX_PLAYERS + 1))


def player_number(value):
    if not isinstance(value, str) or value not in PLAYER_IDS:
        raise ValueError('Invalid room player')
    return int(value[6:])


def validate_nickname(value, *, allow_empty=False):
    if isinstance(value, str) and ((allow_empty and value == '')
                                  or re.fullmatch(r'[A-Za-z0-9_-]{3,16}', value)):
        return value
    raise ValueError('Nickname: 3 to 16 letters, digits, _ or -.')
