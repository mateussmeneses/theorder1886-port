from paths import ROOT
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from patches import (compile_patches, external_patches, external_selection, compile_external,
                     game_profile, selected_patches, patch_requirements, GAMES)

ORDER = ROOT / 'patches/TheOrder1886-Orbis.xml'
ORDER_IDS = {'CUSA00035'}
SEGMENTS = [(0, 0x6000000)]


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA00035</ID></TitleID>
  <Metadata Title="The Order 1886" Name="On" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00001000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="The Order 1886" Name="Off" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00002000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="The Order 1886" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="The Order 1886" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00003000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA00035', 'CUSA99999'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder, '01.02', ORDER, ORDER_IDS)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(ORDER.parent, '01.02', ORDER, ORDER_IDS), [])


class GameProfileTests(unittest.TestCase):
    def test_profiles_by_title_id(self):
        self.assertEqual(game_profile('CUSA00035')[1:3], (ORDER, '01.02'))
        self.assertIsNone(game_profile('CUSA99999'))
        self.assertIsNone(game_profile(None))

    def test_unavailable_frame_rate_patch_falls_back_to_the_highest_lower_one(self):
        from graphics_options import restrict_to_available
        available = {'Skip Intro Logos', 'Frame Rate Patch - 60 FPS'}
        self.assertEqual(restrict_to_available(['Skip Intro Logos', 'Frame Rate Patch - 120 FPS'], available),
                         ['Skip Intro Logos', 'Frame Rate Patch - 60 FPS'])
        self.assertEqual(restrict_to_available(['Aspect Ratio - 32:9'], available), [])

    def test_defaults_and_extras(self):
        # Every built-in patch of The Order is off by default; the launcher's selection is exact.
        self.assertEqual(selected_patches(ORDER, '01.02', ''), [])
        self.assertEqual(selected_patches(ORDER, '01.02', 'Skip Intro Logos; Frame Rate Patch - 60 FPS'),
                         ['Skip Intro Logos', 'Frame Rate Patch - 60 FPS'])
        self.assertEqual(selected_patches(ORDER, '01.02', 'Skip Intro Logos', only=True), ['Skip Intro Logos'])
        self.assertEqual(selected_patches(ORDER, '01.00', '', only=True), [])
        with self.assertRaises(ValueError):
            selected_patches(ORDER, '01.02', 'Aspect Ratio - 16:9;Aspect Ratio - 21:9')

    def test_profile_by_title_for_any_region(self):
        from unittest import mock
        import patches
        self.assertEqual(game_profile('CUSA00070')[1], ORDER)
        self.assertEqual(game_profile('CUSA99999', 'The Order 1886™')[1], ORDER)
        self.assertIsNone(game_profile('CUSA99999', 'Another Game'))
        with mock.patch.object(patches, 'game_sfo', return_value={'TITLE_ID': 'CUSA77777', 'TITLE': 'The Order 1886'}):
            self.assertTrue(patches.game_patchable('folder'))
        self.assertFalse(patches.game_patchable(None))
        with tempfile.TemporaryDirectory() as folder:
            self.assertFalse(patches.game_patchable(folder))  # no param.sfo: unknown game


class SignatureTests(unittest.TestCase):
    XML = """<?xml version="1.0"?>
<Patch>
  <Metadata Name="Found" AppVer="01.02">
    <PatchList><Line Type="bytes" Address="0x1004" Value="9090" Pattern="11 22 ?? 44 55 66" At="1"/></PatchList>
  </Metadata>
  <Metadata Name="Call" AppVer="01.02">
    <PatchList><Line Type="bytes" Address="0x1010" Value="e8{rel:there:0}" Pattern="aa bb cc dd ee" At="0"/></PatchList>
    <Target Name="there" Address="0x1100" Pattern="e9 ?? ?? ?? ?? 77 88" At="1" Ref="1"/>
  </Metadata>
  <Metadata Name="Missing" AppVer="01.02">
    <PatchList>
      <Line Type="bytes" Address="0x1004" Value="9090" Pattern="11 22 ?? 44 55 66" At="1"/>
      <Line Type="bytes" Address="0x1020" Value="90" Pattern="de ad be ef" At="0"/>
    </PatchList>
  </Metadata>
  <Metadata Name="Data" AppVer="01.02">
    <PatchList><Line Type="bytes32" Address="0x2000" Target="variable" Value="0x2"/></PatchList>
    <Target Name="variable" Address="0x2000" Pattern="44 89 35 ?? ?? ?? ?? c6" At="3" Ref="1"/>
  </Metadata>
</Patch>"""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.xml = Path(directory.name) / 'test.xml'
        self.xml.write_text(self.XML)
        image = bytearray(0x400)
        image[0x40:0x46] = bytes.fromhex('112233445566')
        image[0x80:0x85] = bytes.fromhex('aabbccddee')
        image[0x200:0x207] = bytes.fromhex('e9') + (0x300 - 0x205).to_bytes(4, 'little', signed=True) + bytes.fromhex('7788')
        # mov [rip+disp], r14d storing to 0x3f0, then the c6 of the next instruction.
        image[0x280:0x288] = bytes.fromhex('448935') + (0x3f0 - 0x287).to_bytes(4, 'little', signed=True) + b'\xc6'
        self.image = bytes(image)

    def test_sites_found_anywhere_and_rel32_recomputed(self):
        skipped = {}
        writes = compile_patches(self.xml, ['Found', 'Call', 'Missing'], None, [(0, 0x400)], self.image, skipped)
        call = (0x300 - (0x80 + 5)).to_bytes(4, 'little', signed=True)
        self.assertEqual(writes, [(0x41, b'\x90\x90'), (0x80, b'\xe8' + call)])
        # One site of the patch is missing: the whole patch is left out, with the reason.
        self.assertEqual(skipped, {'Missing': 'signature not found'})

    def test_ambiguous_signature_is_skipped(self):
        image = bytearray(self.image)
        image[0x100:0x106] = image[0x40:0x46]
        skipped = {}
        self.assertEqual(compile_patches(self.xml, ['Found'], None, [(0, 0x400)], bytes(image), skipped), [])
        self.assertEqual(skipped, {'Found': 'signature found more than once'})

    def test_without_the_image_the_reference_addresses_are_used(self):
        writes = compile_patches(self.xml, ['Found', 'Call'], '01.02', [(0, 0x10000)])
        self.assertEqual(writes[0], (0x1004, b'\x90\x90'))
        self.assertEqual(writes[1][0], 0x1010)

    def test_data_line_writes_at_its_target(self):
        # The variable has no bytes of its own to find: the code that stores to it points at it.
        self.assertEqual(compile_patches(self.xml, ['Data'], None, [(0, 0x400)], self.image, {}),
                         [(0x3f0, (2).to_bytes(4, 'little'))])
        self.assertEqual(compile_patches(self.xml, ['Data'], '01.02', [(0, 0x10000)]),
                         [(0x2000, (2).to_bytes(4, 'little'))])
        image = bytearray(self.image)
        image[0x280:0x288] = bytes(8)  # store not found: the whole patch is left out
        skipped = {}
        self.assertEqual(compile_patches(self.xml, ['Data'], None, [(0, 0x400)], bytes(image), skipped), [])
        self.assertEqual(skipped, {'Data': 'signature not found'})

    def test_sites_without_a_signature_only_in_the_reference_build(self):
        # Address is the reference build's: in another build it writes into unrelated code (a
        # 01.00 eboot crashed at start, issue #3).
        plain = self.xml.parent / 'plain.xml'
        plain.write_text('<?xml version="1.0"?><Patch><Metadata Name="Plain" AppVer="01.02"><PatchList>'
                         '<Line Type="bytes" Address="0x41" Value="90"/></PatchList></Metadata></Patch>')
        self.assertEqual(compile_patches(plain, ['Plain'], None, [(0, 0x400)], self.image, {}), [(0x41, b'\x90')])
        skipped = {}
        self.assertEqual(compile_patches(plain, ['Plain'], None, [(0, 0x400)], self.image, skipped,
                                         reference=False), [])
        self.assertEqual(skipped, {'Plain': 'no signature (written for the 01.02 eboot only)'})
        # A Line written at a Target found by its signature needs no signature of its own.
        self.assertEqual(compile_patches(self.xml, ['Data'], None, [(0, 0x400)], self.image, {}, reference=False),
                         [(0x3f0, (2).to_bytes(4, 'little'))])

    @unittest.skipUnless((ROOT / 'out/eboot.elf').exists(), 'needs the prepared 01.02 eboot (out/eboot.elf)')
    def test_builtin_signatures_find_the_reference_sites(self):
        # Every built-in patch also applies to other builds by its signatures: on the reference
        # build they must find exactly the sites of its Address values.
        import patches
        elf = (ROOT / 'out/eboot.elf').read_bytes()
        segments = patches.eboot_segments(elf)
        image = patches.eboot_image(elf)
        for meta in ET.parse(ORDER).getroot().iter('Metadata'):
            name = meta.get('Name')
            skipped = {}
            found = compile_patches(ORDER, [name], None, segments, image, skipped, reference=False)
            self.assertEqual(skipped, {}, name)
            self.assertEqual(sorted(found), sorted(compile_patches(ORDER, [name], '01.02', segments)), name)

    @unittest.skipUnless((ROOT / 'out/eboot.elf').exists(), 'needs the prepared 01.02 eboot (out/eboot.elf)')
    def test_game_arguments_patch_points_at_mains_argument_string(self):
        # sub_1fd010 loads the pointer main stores at 0x165ae00 (mov rsi, [rip+rel32]) instead of
        # the fixed "-frontend -syncinterval 2", and main's loop starts at argv[1] with the same
        # exit target 0x298332.
        import struct
        import patches
        elf = (ROOT / 'out/eboot.elf').read_bytes()
        image = bytearray(patches.eboot_image(elf))
        self.assertEqual(bytes(image[0x1482726:0x1482740]), b'-frontend -syncinterval 2\0')
        for offset, data in compile_patches(ORDER, ['Command Line - Game Arguments'], None,
                                            patches.eboot_segments(elf), bytes(image), {}, reference=False):
            image[offset:offset + len(data)] = data
        self.assertEqual(bytes(image[0x1fd12e:0x1fd131]), bytes.fromhex('488b35'))
        self.assertEqual(0x1fd135 + struct.unpack_from('<i', image, 0x1fd131)[0], 0x165ae00)
        self.assertEqual(bytes(image[0x298203:0x298208]), bytes.fromhex('83fb010f8e'))  # cmp ebx,1; jle
        self.assertEqual(0x29820c + struct.unpack_from('<i', image, 0x298208)[0], 0x298332)
        self.assertEqual(bytes(image[0x29820c:0x298210]), bytes.fromhex('6a01415f'))  # push 1; pop r15

    @unittest.skipUnless((ROOT / 'out/eboot.elf').exists(), 'needs the prepared 01.02 eboot (out/eboot.elf)')
    def test_fov_sites_hold_the_original_constant(self):
        # The in-game field of view scales these: the game's own value is 1.35 / 2.4.
        import struct
        import patches
        image = patches.eboot_image((ROOT / 'out/eboot.elf').read_bytes())
        sites = patches.fov_sites(ORDER, '01.02', image, reference=False)
        self.assertEqual(len(sites), 7)
        self.assertEqual({struct.unpack_from('<d', image, s)[0] for s in sites}, {0.5625})

    def test_frame_rate_patch_finds_the_01_00_sites(self):
        # CUSA00076 01.00 (issue #12): the same four sites, one of them through rcx instead of
        # rax (48 c7 81 .. c7 41 where 01.02 has 48 c7 80 .. c7 40).
        sites = ['49 69 97 18 01 00 00 35 82 00 00 48 39 d1 0f',
                 '48 29 ca 48 69 c2 35 82 00 00 e9 15 02 00 00',
                 '48 29 c1 48 69 c1 35 82 00 00 eb 0a',
                 '48 c7 81 80 00 00 00 35 82 00 00 c7 41 7c 01 00 00 00']
        image = bytearray(16)
        constants = []
        for site in sites:
            data = bytes.fromhex(site)
            constants.append(len(image) + data.index(bytes.fromhex('35820000')))
            image += data + bytes(16)
        skipped = {}
        writes = compile_patches(ORDER, ['Frame Rate Patch - 60 FPS'], None, [(0, len(image))],
                                 bytes(image), skipped, reference=False)
        self.assertEqual(skipped, {})
        self.assertEqual(sorted(writes), [(c, bytes.fromhex('1b410000')) for c in sorted(constants)])

    def test_frame_time_sites_show_the_constant_in_context(self):
        import patches
        image = bytes(16) + bytes.fromhex('48 69 c2 35 82 00 00 e9 15 02 00 00') + bytes(8)
        self.assertEqual(patches.frame_time_sites(image),
                         ['0x13: 00 00 00 00 00 00 00 00 00 48 69 c2 [35 82 00 00] e9 15 02 00'])

    def test_requirements_from_patch_notes(self):
        # Notes ask for extra direct memory (shadPS4 "DMEM") and a VBlank rate.
        notes = self.xml.parent / 'notes.xml'
        notes.write_text('''<?xml version="1.0"?>
<Patch>
  <Metadata Name="Big" AppVer="01.02" Note="Requires setting DMEM to 6144 MB."><PatchList/></Metadata>
  <Metadata Name="Fast" AppVer="01.02" Note="Set VBlank to 120 FPS."><PatchList/></Metadata>
</Patch>''')
        self.assertEqual(patch_requirements(notes, ['Big'], '01.02'), (5056 + 6144, None))
        self.assertEqual(patch_requirements(notes, ['Fast'], '01.02'), (None, 120))
        self.assertEqual(patch_requirements(notes, [], '01.02'), (None, None))

    def test_the_order_patches_compile(self):
        names = [m.get('Name') for m in ET.parse(ORDER).getroot().iter('Metadata')]
        self.assertTrue(compile_patches(ORDER, names, '01.02', [(0, 0x1800000)]))


if __name__ == '__main__':
    unittest.main()
