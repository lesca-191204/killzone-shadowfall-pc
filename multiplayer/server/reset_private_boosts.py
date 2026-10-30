"""Reset the private copy's Boost allocation once before starting its game."""
from pathlib import Path
import sys

from local_profile import LocalProfile


def main():
    if len(sys.argv) != 1:
        print('Boost reset does not accept arguments.', file=sys.stderr)
        return 2
    directory = Path(__file__).resolve().parents[1] / 'local-profile'
    try:
        LocalProfile.reset_boosts(directory)
    except (OSError, ValueError) as error:
        print(f'Private Boost reset failed: {error}', file=sys.stderr)
        return 1
    print('Private Boost inventory: 5 of each type for this game launch.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
