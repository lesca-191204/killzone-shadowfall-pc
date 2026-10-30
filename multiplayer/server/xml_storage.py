"""Atomic XML persistence for the private backend stores."""
import os
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

XSI = 'http://www.w3.org/2001/XMLSchema-instance'
ET.register_namespace('xsi', XSI)
for prefix in ('c', 'ch', 'a', 'm', 's', 'context'):
    ET.register_namespace(prefix, prefix)


def atomic_write(filename, root):
    filename = Path(filename)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=filename.parent, suffix='.tmp', delete=False) as output:
            temporary = Path(output.name)
            output.write(ET.tostring(root, encoding='utf-8'))
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, filename)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
