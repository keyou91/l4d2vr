"""Audit locally installed stock firearm assets without changing the installation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import math
import re

from prepare_vr_materials import Vpk

# Native L4D2 IDs and authored viewmodels. Dual pistols are one inventory type.
WEAPONS = (
    (1, 'Pistol', 'v_pistola', 'pistol'),
    (1, 'Dual pistols', 'v_dual_pistola', 'pistol'),
    (2, 'Uzi', 'v_smg', 'magazine'),
    (3, 'Pump shotgun', 'v_pumpshotgun', 'pump'),
    (4, 'Auto shotgun', 'v_autoshotgun', 'tube'),
    (5, 'M16', 'v_rifle', 'magazine'),
    (6, 'Hunting rifle', 'v_huntingrifle', 'magazine'),
    (7, 'Silenced SMG', 'v_silenced_smg', 'magazine'),
    (8, 'Chrome shotgun', 'v_shotgun_chrome', 'pump'),
    (9, 'Desert rifle', 'v_desert_rifle', 'magazine'),
    (10, 'Military sniper', 'v_sniper_military', 'magazine'),
    (11, 'SPAS', 'v_shotgun_spas', 'tube'),
    (21, 'Grenade launcher', 'v_grenade_launcher', 'single_round'),
    (26, 'AK47', 'v_rifle_ak47', 'magazine'),
    (32, 'Magnum', 'v_desert_eagle', 'magazine'),
    (33, 'MP5', 'v_smg_mp5', 'magazine'),
    (34, 'SG552', 'v_rif_sg552', 'magazine'),
    (35, 'AWP', 'v_snip_awp', 'bolt'),
    (36, 'Scout', 'v_snip_scout', 'bolt'),
    (37, 'M60', 'v_m60', 'non_reloadable'),
)


class InstalledAssets:
    def __init__(self, game):
        self.packs = []
        for folder in ('left4dead2', 'left4dead2_dlc1', 'left4dead2_dlc2', 'left4dead2_dlc3', 'update'):
            path = Path(game)/folder/'pak01_dir.vpk'
            if path.is_file():
                self.packs.append(Vpk(path))
        if not self.packs:
            raise ValueError('No installed stock archives found')

    def read(self, name):
        for pack in reversed(self.packs):
            if name.lower() in pack.entries:
                return pack.read(name, limit=32 * 1024 * 1024)
        raise KeyError(name)


def integers(data, offset, count=1):
    if offset < 0 or offset + 4 * count > len(data):
        raise ValueError('Model table outside asset')
    return struct.unpack_from('<' + 'i' * count, data, offset)


def cstring(data, offset):
    if offset < 0 or offset >= len(data):
        raise ValueError('Model string outside asset')
    end = data.find(b'\0', offset, min(offset + 1024, len(data)))
    if end < 0:
        raise ValueError('Unterminated model string')
    return data[offset:end].decode('utf-8', 'surrogateescape')


def metadata(mdl):
    if len(mdl) < 408 or mdl[:4] != b'IDST' or integers(mdl, 4)[0] not in (48, 49):
        raise ValueError('Unsupported studio model')
    if integers(mdl, 76)[0] != len(mdl):
        raise ValueError('Studio model length mismatch')
    bone_count, bone_table = integers(mdl, 156, 2)
    if bone_count < 1 or bone_count > 256:
        raise ValueError('Invalid bone count')
    bones = []
    for index in range(bone_count):
        at = bone_table + index * 216
        name = cstring(mdl, at + integers(mdl, at)[0])
        parent = integers(mdl, at + 4)[0]
        if parent < -1 or parent >= bone_count or parent == index:
            raise ValueError('Invalid bone parent')
        bones.append({'index': index, 'name': name, 'parent': parent})
    for bone in bones:
        seen = set()
        parent = bone['index']
        while parent >= 0:
            if parent in seen:
                raise ValueError('Cyclic bone hierarchy')
            seen.add(parent)
            parent = bones[parent]['parent']
    textures, texture_table, folders, folder_table = integers(mdl, 204, 4)
    if textures < 1 or textures > 256 or folders < 0 or folders > 256:
        raise ValueError('Invalid material count')
    materials = [cstring(mdl, texture_table + i * 64 + integers(mdl, texture_table + i * 64)[0]) for i in range(textures)]
    directories = [cstring(mdl, integers(mdl, folder_table + i * 4)[0]) for i in range(folders)]
    return {'checksum': struct.unpack_from('<I', mdl, 8)[0], 'bones': bones,
            'materials': materials, 'directories': directories}


def geometry(mdl, vvd, vtx, meta):
    if len(vvd) < 64 or vvd[:4] != b'IDSV' or integers(vvd, 4)[0] != 4:
        raise ValueError('Unsupported vertex data')
    if len(vtx) < 36 or integers(vtx, 0)[0] != 7 or vtx[16:20] != mdl[8:12] or vvd[8:12] != mdl[8:12]:
        raise ValueError('Model component checksum/version mismatch')
    count = integers(vvd, 16)[0]
    fixups, fixup_table, vertex_start = integers(vvd, 48, 3)
    if count < 1 or count > 500000 or vertex_start < 64 or vertex_start + count * 48 > len(vvd):
        raise ValueError('Invalid LOD0 vertex range')
    order = list(range(count))
    if fixups:
        if fixups < 0 or fixups > count:
            raise ValueError('Invalid fixup count')
        order = []
        for i in range(fixups):
            lod, first, length = integers(vvd, fixup_table + i * 12, 3)
            if lod < 0 or first < 0 or length < 0 or first + length > count:
                raise ValueError('Invalid vertex fixup')
            order.extend(range(first, first + length))
        if len(order) != count or len(set(order)) != count:
            raise ValueError('Invalid LOD0 fixup coverage')
    vertices = [struct.unpack_from('<3f', vvd, vertex_start + i * 48 + 16) for i in order]
    for original, point in zip(order, vertices):
        at = vertex_start + original * 48
        normal = struct.unpack_from('<3f',vvd,at + 28)
        uv = struct.unpack_from('<2f',vvd,at + 40)
        weights = struct.unpack_from('<3f',vvd,at)
        active = vvd[at + 15]
        ids = vvd[at + 12:at + 15]
        if not all(math.isfinite(v) for v in (*point,*normal,*uv,*weights)):
            raise ValueError('Non-finite vertex contents')
        if active < 1 or active > 3 or any(w < 0 or w > 1.001 for w in weights) or abs(sum(weights[:active]) - 1.0) > 0.005:
            raise ValueError('Invalid vertex skin weights')
        if any(ids[i] >= len(meta['bones']) for i in range(active)):
            raise ValueError('Vertex skin bone outside rig')
    body_count, body_table = integers(mdl, 232, 2)
    if body_count < 1 or body_count > 64 or integers(vtx, 28)[0] != body_count:
        raise ValueError('Invalid/mismatched model bodyparts')
    vtx_body_table = integers(vtx, 32)[0]
    parts = []
    for body in range(body_count):
        bp, vb = body_table + body * 16, vtx_body_table + body * 8
        model_count, _, model_offset = integers(mdl, bp + 4, 3)
        vm_count, vm_offset = integers(vtx, vb, 2)
        if model_count < 1 or model_count > 64 or vm_count != model_count:
            raise ValueError('Invalid bodypart variants')
        for variant in range(model_count):
            ms, vm = bp + model_offset + variant * 148, vb + vm_offset + variant * 8
            mesh_count, mesh_offset, _, model_vertex_offset = integers(mdl, ms + 72, 4)
            lod_count, lod_offset = integers(vtx, vm, 2)
            if lod_count < 1 or lod_count > 8:
                if mesh_count == 0:
                    continue
                raise ValueError('Invalid model LOD count')
            vl = vm + lod_offset
            vm_mesh_count, vm_mesh_offset = integers(vtx, vl, 2)
            if mesh_count < 0 or mesh_count > 256 or mesh_count != vm_mesh_count:
                raise ValueError('Invalid/mismatched LOD0 meshes')
            for mesh in range(mesh_count):
                mm, mv = ms + mesh_offset + mesh * 116, vl + vm_mesh_offset + mesh * 9
                material, _, _, mesh_vertex_offset = integers(mdl, mm, 4)
                if material < 0 or material >= len(meta['materials']):
                    raise ValueError('Invalid mesh material index')
                groups, group_offset = integers(vtx, mv, 2)
                if groups < 0 or groups > 4096:
                    raise ValueError('Invalid strip-group count')
                faces = []
                for group in range(groups):
                    sg = mv + group_offset + group * 25
                    nverts, voffset, nindices, ioffset, nstrips, soffset = integers(vtx, sg, 6)
                    if not (0 <= nverts <= count and 0 <= nindices <= 3000000 and 0 <= nstrips <= 100000):
                        raise ValueError('Invalid strip-group sizes')
                    lookup = [struct.unpack_from('<H', vtx, sg + voffset + i * 9 + 4)[0] + mesh_vertex_offset + model_vertex_offset // 48 for i in range(nverts)]
                    indices = struct.unpack_from('<' + 'H' * nindices, vtx, sg + ioffset)
                    for strip in range(nstrips):
                        sh = sg + soffset + strip * 27
                        size, first = integers(vtx, sh, 2)
                        if first < 0 or size < 0 or first + size > nindices:
                            raise ValueError('Invalid strip index range')
                        sequence = [lookup[i] for i in indices[first:first + size]]
                        flags = vtx[sh + 18]
                        if flags & 1:
                            faces.extend(tuple(sequence[i:i + 3]) for i in range(0, len(sequence) - 2, 3))
                        elif flags & 2:
                            faces.extend(tuple(sequence[i:i + 3]) for i in range(len(sequence) - 2))
                        else:
                            raise ValueError('Unsupported strip topology')
                faces = [f for f in faces if len(set(f)) == 3]
                if any(i < 0 or i >= count for face in faces for i in face):
                    raise ValueError('Triangle vertex outside LOD0')
                used = sorted({i for face in faces for i in face})
                edges = {}
                for face in faces:
                    welded = [tuple(round(v, 4) for v in vertices[i]) for i in face]
                    for a, b in zip(welded, welded[1:] + welded[:1]):
                        key = tuple(sorted((a, b)))
                        edges[key] = edges.get(key, 0) + 1
                parts.append({'body': body, 'variant': variant, 'mesh': mesh,
                    'material': meta['materials'][material], 'triangles': len(faces),
                    'open_edges': sum(n == 1 for n in edges.values()),
                    'bounds': [[min(vertices[i][a] for i in used) for a in range(3)],
                               [max(vertices[i][a] for i in used) for a in range(3)]] if used else None})
    return {'lod0_vertices': count, 'parts': parts}


def audit(game):
    assets = InstalledAssets(game)
    result = {'stock_only': True, 'runtime_verified': False, 'models': []}
    for weapon_id, label, stem, reload_kind in WEAPONS:
        name = 'models/v_models/' + stem + '.mdl'
        record = {'weapon_id': weapon_id, 'label': label, 'model': name, 'reload_kind': reload_kind}
        try:
            mdl, vvd, vtx = [assets.read(name[:-4] + suffix) for suffix in ('.mdl', '.vvd', '.dx90.vtx')]
            meta = metadata(mdl)
            record.update(checksum=meta['checksum'], bone_count=len(meta['bones']),
                component_sha256={suffix: hashlib.sha256(data).hexdigest() for suffix, data in zip(('mdl','vvd','vtx'), (mdl,vvd,vtx))},
                **geometry(mdl, vvd, vtx, meta))
            material_paths = []
            for material in meta['materials']:
                for folder in meta['directories']:
                    path = ('materials/' + folder.replace('\\','/') + material + '.vmt').lower()
                    try:
                        text = assets.read(path).decode('utf-8-sig', 'surrogateescape')
                    except KeyError:
                        continue
                    textures = []
                    for match in re.finditer(r'(?im)^\s*"?\$(?:basetexture|bumpmap|detail|envmapmask|phongexponenttexture)"?\s+"?([^"\s{}]+)',text):
                        texture_path = 'materials/' + match.group(1).replace('\\','/').lower() + '.vtf'
                        texture = assets.read(texture_path)
                        if len(texture) < 64 or texture[:4] != b'VTF\0':
                            raise ValueError('Invalid texture: ' + texture_path)
                        width,height = struct.unpack_from('<HH',texture,16)
                        if not (1 <= width <= 8192 and 1 <= height <= 8192):
                            raise ValueError('Invalid texture dimensions: ' + texture_path)
                        textures.append({'path': texture_path, 'width':width, 'height':height,
                            'sha256':hashlib.sha256(texture).hexdigest()})
                    material_paths.append({'path': path, 'shader': text.split('{',1)[0].strip(), 'textures': textures,
                        'two_sided': '"$nocull" "1"' in text or '$nocull 1' in text})
                    break
                else:
                    raise ValueError('Unresolved material: ' + material)
            record.update(materials=material_paths, structural_valid=True,
                per_mesh_closed=all(p['open_edges'] == 0 for p in record['parts']))
        except (KeyError, ValueError, struct.error, IndexError) as error:
            record.update(structural_valid=False, error=str(error))
        result['models'].append(record)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.game)
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'models': len(report['models']),
        'valid': sum(m['structural_valid'] for m in report['models']),
        'open_mesh_models': sum(m.get('structural_valid', False) and not m['per_mesh_closed'] for m in report['models']),
        'failures': [{'model':m['model'],'error':m['error']} for m in report['models'] if not m['structural_valid']]}))
