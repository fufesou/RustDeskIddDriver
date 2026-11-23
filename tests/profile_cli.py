"""Smoke tests for the packaged controller. Run with RustDeskIddApp.exe as the first argument."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

APP = pathlib.Path(sys.argv.pop(1)).resolve()


class DisplayProfileTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.profile = pathlib.Path(temp.name) / "gaming.profile"

    def command(self, *args):
        return subprocess.run([str(APP), *map(str, args)], stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=3)

    def write_profile(self, modes, extra=""):
        self.profile.write_text(
            "connector=0\nid={C26C0A51-A356-4D96-ACD5-DD624A0A2BE9}\n"
            + "".join(f"mode={mode}\n" for mode in modes) + extra,
            encoding="utf-8")

    def test_fractional_and_high_refresh(self):
        self.write_profile(["3840x2160@120", "1920x1080@60000/1001", "1920x1080@60"])
        result = self.command("profile", "validate", self.profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("60000/1001", result.stdout)
        self.assertIn("3840x2160@120", result.stdout)

    def test_generated_refresh_rates_survive_preference_changes(self):
        modes = ["160x90@1/10", "160x90@11/100", "1280x720@60/1", "160x90@54259519/123457"]
        for preferred in modes:
            ordered = [preferred] + [mode for mode in modes if mode != preferred]
            with self.subTest(preferred=preferred):
                self.write_profile(ordered)
                result = self.command("profile", "validate", self.profile)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines()[1:], ordered)

    def test_invalid_modes(self):
        for mode in ["16385x1080@60", "1920x0@60", "1920x1080@1/0", "1920x1080@-1",
                     "1920x1080@60junk", "4294967295x4294967295@4294967295"]:
            with self.subTest(mode=mode):
                self.write_profile([mode])
                self.assertNotEqual(self.command("profile", "validate", self.profile).returncode, 0)

    def test_preferred_clock_outside_base_edid(self):
        for preferred in ("16384x16384@625", "160x90@1/10"):
            for modes in ([preferred], [preferred, "1280x720@60"]):
                with self.subTest(modes=modes):
                    self.write_profile(modes, "hdr=1\n")
                    result = self.command("profile", "validate", self.profile)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = [mode if "/" in mode else mode + "/1" for mode in modes]
                    self.assertEqual(result.stdout.splitlines()[1:], expected)

    def test_equivalent_rates_are_duplicates(self):
        self.write_profile(["1920x1080@60", "1920x1080@120/2"])
        self.assertNotEqual(self.command("profile", "validate", self.profile).returncode, 0)

    @staticmethod
    def base_edid(revision=4, standard="01 01", major=1, *, cta=b""):
        # Base EDID with a 1280x720 preferred timing and two unused descriptors.
        edid = bytearray(128)
        edid[:38] = bytes.fromhex("00 ff ff ff ff ff ff 00 4a 74 01 00 00 00 00 00 00 22 01 04"
                                  " 80 00 00 78 06 ee 91 a3 54 4c 99 26 0f 50 54 00 00 00")
        edid[38:54] = bytes.fromhex(standard) + b"\x01\x01" * 7
        edid[54:72] = bytes.fromhex("d9 17 00 5f 50 d0 14 20 08 20 35 00 00 00 00 00 00 1e")
        edid[72:90] = b"\0\0\0\xfc\0RustDesk IDD\n"
        edid[93] = edid[111] = 0x10  # Unused descriptors.
        edid[18:20] = major, revision
        if cta:
            edid[126] = 1
            edid.extend((bytes((2, 3, 4 + len(cta), 0)) + cta).ljust(128, b"\0"))
        return edid

    def validate_edid(self, edid, modes):
        data = bytearray(edid)
        for pos in range(0, len(data), 128):
            data[pos + 127] = -sum(data[pos:pos + 127]) & 0xff
        self.profile.with_name("monitor.bin").write_bytes(data)
        self.write_profile(modes, "edid=monitor.bin\n")
        return self.command("profile", "validate", self.profile)

    def assert_edid_error(self, edid, modes, *messages):
        result = self.validate_edid(edid, modes)
        self.assertNotEqual(result.returncode, 0)
        for message in messages:
            self.assertIn(message, result.stderr)

    def test_edid_standard_timing_aliases(self):
        cvt, gtf = (220_750_000, 2608 * 1130), (220_637_000, 2608 * 1128)
        for revision, limits, code, mode, (clock, total) in (
                (4, None, "d1 cf", "1920x1080@75", cvt),
                (4, "01 0a 20 20 20 20 20 20", "d1 cf", "1920x1080@75", cvt),
                (4, "04 11 00 f0 38 18 00 3c", "d1 cf", "1920x1080@75", cvt),
                (4, "00 0a 20 20 20 20 20 20", "d1 cf", "1920x1080@75", gtf),
                (4, "02 00 7d 50 58 02 80 28", "d1 cf", "1920x1080@75", gtf),
                (3, None, "d1 cf", "1920x1080@75", gtf),
                (2, None, "d1 cf", "1920x1080@75", gtf),
                (4, None, "01 40", "256x192@60", (3_750_000, 320 * 206)),
                (4, None, "01 01", "256x160@61", (3_250_000, 320 * 176))):
            with self.subTest(revision=revision, limits=limits, mode=mode):
                edid = self.base_edid(revision, code)
                if limits:
                    edid[108:126] = bytes.fromhex("00 00 00 fd 00 32 78 1e c8 3c " + limits)
                exact = f"{mode.split('@')[0]}@{clock}/{total}"
                if code == "01 01":  # An unused slot must not alias either rate.
                    result = self.validate_edid(edid, [mode, exact])
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assert_edid_error(edid, [mode, exact], "Duplicate display modes")

    def test_cvt_preference_and_physical_aliases(self):
        reduced = f"1600x1000@{108_500_000}/{1760 * 1029}"
        normal = f"1600x1000@{132_250_000}/{2128 * 1038}"

        def codes(flags):
            return b"".join(bytes((0xf3, 0x18, value)) for value in flags)

        # 0x29 prefers reduced 60 Hz. 0x1d prefers 50 Hz and also offers both 60 Hz variants.
        for standard, base_flags, ext_flags, prefer_reduced in (
                ("01 01", [0x29], [], True), ("a9 00", [0x29], [], True), ("a9 00", [], [0x29], True),
                ("a9 00", [0x1d], [0x29], False), ("a9 00", [0x29], [0x1d], True),
                ("01 01", [0x1d, 0x29], [], False), ("01 01", [], [0x29, 0x1d], True)):
            edid = self.base_edid(standard=standard)
            if base_flags:
                edid[90:108] = (bytes.fromhex("00 00 00 f8 00 01") + codes(base_flags)).ljust(18, b"\0")
            if ext_flags:
                edid[126] = 1
                edid.extend((bytes((0x10, 1, 0, len(ext_flags), 0)) + codes(ext_flags)).ljust(128, b"\0"))
            preferred, other = (reduced, normal) if prefer_reduced else (normal, reduced)
            with self.subTest(standard=standard, base=base_flags, vtb=ext_flags):
                self.assert_edid_error(edid, ["1600x1000@60", preferred], "Duplicate display modes")
                distinct = self.validate_edid(edid, ["1600x1000@60", other])
                self.assertEqual(distinct.returncode, 0, distinct.stderr)

    def test_standard_and_established_priority(self):
        reduced = f"1400x1050@{101_000_000}/{1560 * 1080}"
        normal = f"1400x1050@{121_750_000}/{1864 * 1089}"
        for location, preferred, other in (("absent", reduced, normal), ("base", normal, reduced), ("descriptor", normal, reduced)):
            edid = self.base_edid(standard="90 40" if location == "base" else "01 01")
            # Established Timings III bits for both 60 Hz variants, in reduced-then-normal order.
            edid[90:108] = bytes.fromhex("00 00 00 f7 00 0a 00 00 06").ljust(18, b"\0")
            if location == "descriptor":
                edid[108:126] = bytes.fromhex("00 00 00 fa 00 90 40") + b"\x01\x01" * 5 + b"\x0a"
            with self.subTest(location=location):
                self.assert_edid_error(edid, ["1400x1050@60", preferred], "Duplicate display modes")
                distinct = self.validate_edid(edid, ["1400x1050@60", other])
                self.assertEqual(distinct.returncode, 0, distinct.stderr)

    def test_unusable_gtf_only_affects_dependent_modes(self):
        edid = self.base_edid(3, "01 c0")
        for mode, supported in (("1280x720@60", True), ("256x144@61", True), ("256x144@120/2", False)):
            with self.subTest(mode=mode):
                result = self.validate_edid(edid, [mode])
                self.assertEqual(result.returncode == 0, supported, result.stderr)
                self.assertIn("GTF", result.stderr)
                self.assertIn("256x144@60", result.stderr)
        # A detailed timing independently supplies the requested 256x144 at exactly 60 Hz.
        edid[90:108] = bytes.fromhex("b0 04 00 20 12 90 6a 00 08 20 35 00 00 00 00 00 00 1e")
        result = self.validate_edid(edid, ["256x144@60"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GTF", result.stderr)
        edid[126] = 1
        edid.extend(bytes.fromhex("10 01 ff 00 00").ljust(128, b"\0"))
        self.assert_edid_error(edid, ["1280x720@60"], "Invalid EDID")

    def test_unusable_cvt_and_malformed_cvt_fields(self):
        edid = self.base_edid()
        edid[90:108] = bytes.fromhex("00 00 00 f8 00 01 03 00 28").ljust(18, b"\0")
        good = self.validate_edid(edid, ["1280x720@60"])
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertIn("CVT", good.stderr)
        self.assert_edid_error(edid, ["8x8@60"], "No usable EDID timing for 8x8@60")
        edid[98] |= 0x80  # Reserved bit in the CVT code.
        self.assert_edid_error(edid, ["1280x720@60"], "Invalid EDID")

    def test_unusable_detailed_timings_cannot_supply_rgb(self):
        dtd = bytes.fromhex("08 e8 00 30 f2 70 5a 80 1c 58 8a 80 00 00 00 00 00 1e")
        vii = bytes.fromhex("f6 22 02 4f 10 09 04 ff 0e 2f 02 1b 02 57 00 6f 08 59 00 07 00 09 00")
        for base, extension in ((dtd, b""), (b"", vii)):  # Both sync pulses end outside blanking.
            edid = self.base_edid(cta=bytes.fromhex("e2 0e 60") + extension)  # Only VIC 96 (50 Hz) requires 4:2:0.
            edid[90:90 + len(base)] = base
            self.assert_edid_error(edid, ["3840x2160@60"], "No usable EDID timing", "invalid sync")
            edid[128 + 6] = 97  # The requested 60 Hz now requires 4:2:0.
            self.assert_edid_error(edid, ["3840x2160@60"], "YCbCr 4:2:0")
            edid[54:72] = bytes.fromhex("4e ed 00 94 f2 70 5a 80 b0 58 8a 00 00 00 00 00 00 1f")
            self.assertEqual(self.validate_edid(edid, ["3840x2160@60"]).returncode, 0)
            edid[71] |= 0x20  # The same DTD now requires a stereo presentation.
            self.assert_edid_error(edid, ["3840x2160@60"], "YCbCr 4:2:0")

    def test_detailed_timing_skips_unusable_scan_rates(self):
        # Both 5 GHz records have the same frame rate. Only the second line rate fits a DWORD.
        invalid = bytes.fromhex("3f 4b 4c 08 ff 1d 40 01 07 00 1f 00 df 10 1f 03 02 00 04 00")
        valid = bytes.fromhex("3f 4b 4c 48 ff 1d b7 05 07 00 1f 00 df 10 9f 00 02 00 04 00")  # Mono or stereo.
        stereo = valid[:3] + b"\x28" + valid[4:]  # Stereo only.
        rates = ["7680x4320@" + rate for rate in ("1953125/16002", "39062501/320040", "122")]
        for records in ((invalid, valid), (valid, invalid), (valid,), (invalid,), (invalid, stereo)):
            edid = self.base_edid(cta=b"".join(bytes.fromhex("f6 22 02") + record for record in records))
            for mode in rates:
                with self.subTest(records=records, mode=mode):
                    result = self.validate_edid(edid, [mode])
                    self.assertEqual(result.returncode == 0, valid in records, result.stderr)
                    if valid not in records:
                        self.assertIn("frequency cannot be represented", result.stderr)
            if valid in records:
                for alias in rates[1:]:
                    self.assertIn("Duplicate display modes", self.validate_edid(edid, [rates[0], alias]).stderr)

    def test_formula_timing_requires_sync_and_porches(self):
        cvt = self.base_edid()
        cvt[90:108] = bytes.fromhex("00 00 00 f8 00 01 17 00 28").ljust(18, b"\0")
        for edid, size, fraction, formula in ((self.base_edid(3, "09 c0"), "320x180", "235625/3927", "GTF"),
                                              (cvt, "64x48", "3125/62", "CVT")):
            for rate in ("60", fraction):
                with self.subTest(formula=formula, rate=rate):
                    self.assert_edid_error(edid, [f"{size}@{rate}"], "No usable EDID timing", formula, "sync")
            self.assertEqual(self.validate_edid(edid, ["1280x720@60"]).returncode, 0)
        cvt[98] |= 1  # Reduced blanking independently supplies 64x48 at 60 Hz.
        self.assertEqual(self.validate_edid(cvt, ["64x48@60"]).returncode, 0)

    def test_standard_timing_needs_a_representable_scan_rate(self):
        exact = f"1920x1080@{220_637_000}/{2608 * 1128}"  # VTB standard timing uses default GTF.
        for limits, reason in (("02 00 01 ba 0c 00 80 d5", "Windows"), ("03 00 00 00 00 00 00 00", "unknown")):
            edid = self.base_edid(standard="d1 cf")
            edid[108:126] = bytes.fromhex("00 00 00 fd 00 32 78 1e c8 3c " + limits)
            with self.subTest(limits=limits):
                good = self.validate_edid(edid, ["1280x720@60"])
                self.assertEqual(good.returncode, 0, good.stderr)
                self.assertIn(reason, good.stderr)
                self.assert_edid_error(edid, ["1920x1080@75"], "No usable EDID timing for 1920x1080@75")
                edid[126] = 1
                edid.extend(bytes.fromhex("02 03 07 00 e2 78 02").ljust(128, b"\0"))  # HF-EEODB declares two extensions.
                self.assert_edid_error(edid, ["1280x720@60"], "Invalid EDID")
                edid.extend(bytes.fromhex("10 01 00 00 01 d1 cf").ljust(128, b"\0"))
                self.assert_edid_error(edid, ["1920x1080@75", exact], "Duplicate display modes")
        # The vertical fraction fits a DWORD, but the corresponding line frequency does not.
        edid = self.base_edid(standard="d1 d9")
        edid[108:126] = bytes.fromhex("00 00 00 fd 00 32 78 1e c8 3c 02 00 01 c4 01 00 e0 dc")
        self.assert_edid_error(edid, ["1920x1080@649172875/7637328"], "No usable EDID timing")

    def test_init_keeps_existing_identity(self):
        self.assertEqual(self.command("profile", "init", self.profile, "1").returncode, 0)
        original = self.profile.read_bytes()
        self.assertNotEqual(self.command("profile", "init", self.profile, "1").returncode, 0)
        self.assertEqual(self.profile.read_bytes(), original)
        self.assertEqual(self.command("profile", "validate", self.profile).returncode, 0)

    def test_disabled_profile_accepts_disconnected_edid(self):
        self.write_profile([], "enabled=0\nedid=offline.bin\n")
        result = self.command("profile", "validate", self.profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("disabled", result.stdout)

    def test_invalid_boolean_and_unknown_options(self):
        for field in ["hdr=yes", "cursor=2", "colour=10", "connector=1"]:
            with self.subTest(field=field):
                self.write_profile(["1920x1080@60"], field + "\n")
                self.assertNotEqual(self.command("profile", "validate", self.profile).returncode, 0)

    def test_edid_errors_are_visible(self):
        self.write_profile(["2560x1440@144"], "edid=monitor.bin\n")
        self.assertNotEqual(self.command("profile", "validate", self.profile).returncode, 0)
        self.assert_edid_error(bytes(128), ["2560x1440@144"], "Invalid EDID")
        self.assert_edid_error(self.base_edid(major=2), ["2560x1440@144"], "Unsupported EDID major version 2")

    def test_address_idle_adapter_by_instance_id(self):
        self.write_profile(["1920x1080@60"])
        missing = r"ROOT\RUSTDESK_IDD_TEST\ABSENT"
        result = self.command("adapter", "list")
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in (("gpu", "set", "auto"), ("profile", "apply", self.profile)):
            with self.subTest(command=command):
                result = self.command(*command, "--adapter", missing)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("adapter instance", result.stderr)

    def test_hdr_capture_rejects_sdr(self):
        self.write_profile(["1920x1080@60"])
        output = self.profile.with_suffix(".png")
        result = self.command("hdr-capture", self.profile, output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("enabled HDR profile", result.stderr)
        self.assertFalse(output.exists())

    def test_modes_requires_profile_connector(self):
        self.profile.write_text("id={C26C0A51-A356-4D96-ACD5-DD624A0A2BE9}\nedid=offline.bin\n",
                                encoding="utf-8")
        result = self.command("modes", self.profile)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing connector", result.stderr)


if __name__ == "__main__":
    unittest.main()
