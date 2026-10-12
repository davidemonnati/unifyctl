"""Validate mock exports using an independent JSON parser; no hardware access."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    subprocess.run([str(Path(sys.argv[1]).resolve()), directory], check=True)
    root = Path(directory)
    assert not list(root.glob('.unifyctl-export-*')), 'temporary files leaked'
    for name, slots in [('empty', []), ('sparse', [1, 6]), ('full', list(range(1, 7))),
                        ('optional', [1]), ('encoding', [1]), ('ordering', [1])]:
        data = json.loads((root / f'{name}.json').read_text(encoding='utf-8'))
        assert data['schema_version'] == 1
        assert data['kind'] == 'unifyctl-inventory'
        assert data['restorable_pairings'] is False
        assert data['receiver'] == {
            'family': 'unifying', 'usb_vendor_id': '046d',
            'usb_product_id': 'c52b', 'path': 'test/receiver', 'slot_capacity': 6,
        }
        assert [d['slot'] for d in data['devices']] == slots
        for device in data['devices']:
            assert device['connectivity'] == 'unknown'
            if name == 'optional':
                assert device['serial'] is None and device['name'] is None
            elif name == 'encoding':
                assert device['wpid'] == 'ffff' and device['serial'] == 'ffffffff'
                assert device['type'] == 255
                assert device['name'] == '"\\?é🐭' + '\ufffd' * 4 + 'X'
            else:
                assert device['wpid'] == '4001' and device['serial'] == '12345678'
                assert device['type'] == 2 and device['name'] == 'Mouse'
    assert (root / 'symlink.json').is_symlink()
    assert (root / 'directory').is_dir()
    assert not (root / 'failed.json').exists()
print('Export JSON: independent parsing and schema checks passed')
