"""Prepare local two-sided firearm materials from an installed copy of L4D2.

No Valve assets are bundled with this script. It never launches the game or
changes the installation. Existing output files are preserved.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import tempfile
import zlib


class Vpk:
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open('rb') as source:
            signature, version, length = struct.unpack('<III', source.read(12))
            if signature != 0x55AA1234 or version not in (1, 2) or length > 32 * 1024 * 1024:
                raise ValueError('Unsupported VPK header')
            header_size = 12 if version == 1 else 28
            source.seek(header_size)
            tree = source.read(length)
        if len(tree) != length:
            raise ValueError('Truncated VPK tree')
        self.data_offset = header_size + length
        self.entries = {}
        cursor = 0

        def string():
            nonlocal cursor
            end = tree.index(0, cursor)
            value = tree[cursor:end].decode('utf-8', 'surrogateescape')
            cursor = end + 1
            return value

        while True:
            extension = string()
            if not extension:
                break
            while True:
                folder = string()
                if not folder:
                    break
                while True:
                    name = string()
                    if not name:
                        break
                    crc, preload, archive, offset, size, terminator = struct.unpack_from('<IHHIIH', tree, cursor)
                    cursor += 18
                    if terminator != 0xffff or cursor + preload > len(tree):
                        raise ValueError('Malformed VPK entry')
                    head = tree[cursor:cursor + preload]
                    cursor += preload
                    key = ('' if folder == ' ' else folder + '/') + name + '.' + extension
                    self.entries[key.lower()] = (archive, offset, size, head, crc)

    def read(self, key, limit=1024 * 1024):
        archive, offset, size, preload, crc = self.entries[key.lower()]
        if size + len(preload) > limit:
            raise ValueError('Material exceeds extraction limit')
        if archive == 0x7fff:
            path = self.path
            offset += self.data_offset
        else:
            path = self.path.with_name(f'{self.path.stem.removesuffix("_dir")}_{archive:03d}.vpk')
        with path.open('rb') as source:
            source.seek(offset)
            body = source.read(size)
        content = preload + body
        if len(body) != size or zlib.crc32(content) != crc:
            raise ValueError('Material size or CRC mismatch')
        return content


def two_sided(text):
    text = text.replace('\r\n', '\n').replace('\r', '\n')
    # Only alter a top-level shader material. A patch material needs its base
    # include resolved first; silently injecting a flag there is unreliable.
    if not re.match(r'\s*"?VertexLitGeneric"?\s*\{', text, re.IGNORECASE):
        raise ValueError('Expected a VertexLitGeneric weapon material')
    pattern = r'(?im)^[ \t]*"?\$nocull"?[ \t]+"?\d+"?[ \t]*(?=(?://[^\n]*)?$)'
    if re.search(pattern, text):
        return re.sub(pattern, '\t"$nocull" "1"', text)
    return text.replace('{', '{\n\t"$nocull" "1"', 1)


def prepare(vpk, output, materials=None):
    materials = materials if materials is not None else (
        'materials/models/v_models/weapons/pistol/v_4pistols.vmt',
        'materials/models/v_models/weapons/deserteagle/deserteagle.vmt',
    )
    output = Path(output).resolve()
    staged = []
    for name in materials:
        target = (output / name).resolve()
        if not target.is_relative_to(output) or target.exists():
            raise ValueError(f'Output already exists or is unsafe: {name}')
        text = vpk.read(name).decode('utf-8-sig')
        staged.append((name, target, two_sided(text)))
    for _, target, text in staged:
        target.parent.mkdir(parents=True, exist_ok=True)
        # Do not replace a file created by another writer after preflight.
        with target.open('x', encoding='utf-8', newline='') as destination:
            destination.write(text)
    return [name for name, _, _ in staged]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--vpk', type=Path)
    source.add_argument('--game', type=Path)
    parser.add_argument('--all-weapons', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix='l4d2vr-materials-'))
    if args.all_weapons:
        if args.game is None:
            parser.error('--all-weapons requires --game to resolve DLC/update assets')
        from audit_weapon_assets import InstalledAssets, WEAPONS, metadata
        assets = InstalledAssets(args.game)
        materials = set()
        for _, _, stem, _ in WEAPONS:
            model = metadata(assets.read('models/v_models/' + stem + '.mdl'))
            for material in model['materials']:
                if any(word in material.lower() for word in ('arm', 'hand', 'sleeve')):
                    continue
                for folder in model['directories']:
                    name = ('materials/' + folder.replace('\\', '/') + material + '.vmt').lower()
                    try:
                        assets.read(name)
                    except KeyError:
                        continue
                    materials.add(name)
                    break
                else:
                    raise ValueError('Unresolved weapon material: ' + material)
        files = prepare(assets, output, sorted(materials))
    else:
        if args.vpk is None:
            parser.error('--game requires --all-weapons')
        files = prepare(Vpk(args.vpk), output)
    print(json.dumps({'output': str(output), 'files': files}, indent=2))
