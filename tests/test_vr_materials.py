import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location('vr_materials', Path(__file__).parents[1] / 'tools/prepare_vr_materials.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MaterialsTests(unittest.TestCase):
    def test_existing_output_is_preserved_before_any_write(self):
        class FixtureVpk:
            def read(self, key):
                return b'VertexLitGeneric\n{\n"$basetexture" "fixture"\n}\n'

        with tempfile.TemporaryDirectory(prefix='l4d2vr-material-output-test-') as directory:
            output = Path(directory)
            protected = output / 'materials/models/v_models/weapons/deserteagle/deserteagle.vmt'
            protected.parent.mkdir(parents=True)
            protected.write_text('user material', encoding='utf-8')
            with self.assertRaises(ValueError):
                module.prepare(FixtureVpk(), output)
            self.assertEqual(protected.read_text(encoding='utf-8'), 'user material')
            self.assertFalse((output / 'materials/models/v_models/weapons/pistol/v_4pistols.vmt').exists())

    def test_preserves_shader_parameters_and_is_idempotent(self):
        text = '"VertexLitGeneric"\n{\n\t"$basetexture" "weapons/pistol"\n}\n'
        modified = module.two_sided(text)
        self.assertIn('"$basetexture" "weapons/pistol"', modified)
        self.assertEqual(modified.count('"$nocull"'), 1)
        self.assertEqual(module.two_sided(modified), modified)

    def test_replaces_disabled_culling_flag(self):
        self.assertIn('"$nocull" "1"', module.two_sided('VertexLitGeneric\n{\n$nocull 0\n}\n'))

    def test_rejects_unresolved_patch_material(self):
        with self.assertRaises(ValueError):
            module.two_sided('Patch { include "other" }')

    def test_legacy_line_endings_and_disabled_flag_comment(self):
        original = 'VertexLitGeneric\r\r\n{\r\r\n$nocull 0 // authored flag\r\r\n}\r\r\n'
        result = module.two_sided(original)
        self.assertEqual(result.count('$nocull'), 1)
        self.assertIn('"$nocull" "1"', result)
        self.assertIn('// authored flag', result)
        self.assertEqual(module.two_sided(result), result)

    def test_bulk_preflight_preserves_unrelated_existing_material(self):
        class FixtureVpk:
            def read(self,key):
                return b'VertexLitGeneric\n{\n$basetexture "fixture"\n}\n'
        with tempfile.TemporaryDirectory(prefix='l4d2vr-all-materials-') as directory:
            output = Path(directory)
            (output/'materials').mkdir()
            protected = output/'materials/last.vmt'
            protected.write_text('custom',encoding='utf-8')
            with self.assertRaises(ValueError):
                module.prepare(FixtureVpk(),output,['materials/first.vmt','materials/last.vmt'])
            self.assertFalse((output/'materials/first.vmt').exists())
            self.assertEqual(protected.read_text(),'custom')

    def test_vpk_preload_and_crc(self):
        content = b'hello material'
        entry = struct.pack('<IHHIIH', zlib.crc32(content), 2, 0x7fff, 0, len(content) - 2, 0xffff)
        tree = b'vmt\0materials/test\0item\0' + entry + content[:2] + b'\0\0\0'
        with tempfile.TemporaryDirectory(prefix='l4d2vr-vpk-test-') as directory:
            path = Path(directory) / 'pak01_dir.vpk'
            path.write_bytes(struct.pack('<III', 0x55AA1234, 1, len(tree)) + tree + content[2:])
            self.assertEqual(module.Vpk(path).read('materials/test/item.vmt'), content)
            path.write_bytes(path.read_bytes()[:-1] + b'x')
            with self.assertRaises(ValueError):
                module.Vpk(path).read('materials/test/item.vmt')

    def test_legacy_archive_name_does_not_block_other_materials(self):
        content = b'fixture'
        entry = struct.pack('<IHHIIH',zlib.crc32(content),len(content),0x7fff,0,0,0xffff)
        tree = b'vmt\0materials/test\0legacy\xe9\0' + entry + content + b'\0\0\0'
        with tempfile.TemporaryDirectory(prefix='l4d2vr-vpk-legacy-') as directory:
            path = Path(directory)/'pak01_dir.vpk'
            path.write_bytes(struct.pack('<III',0x55AA1234,1,len(tree)) + tree)
            self.assertEqual(module.Vpk(path).read('materials/test/legacy\udce9.vmt'),content)


if __name__ == '__main__':
    unittest.main()
