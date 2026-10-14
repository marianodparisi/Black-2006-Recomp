"""Address layout checks for the supported retail releases."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import black_release
import generate
from black_release import NTSC, PAL


def test_detect_by_hash():
    assert black_release.detect(NTSC.sha256.lower()) is NTSC
    assert black_release.detect(PAL.sha256) is PAL
    assert black_release.detect("0" * 64) is None


def test_ntsc_is_identity():
    for va in (0x2BA70, 0x115B40, 0x116000, 0x2400E0, 0x2F3A38, 0x7FFFFFFF):
        assert NTSC.to_guest(va) == va
        assert NTSC.to_canonical(va) == va
    text = "RECOMP_ABI_CALL(0x0021C7EFu, sub_0021C7EF); loc_00116000: ;"
    assert NTSC.rename_symbols(text) == text
    assert NTSC.guest_symbols(text) == text


def test_pal_layout_matches_verified_addresses():
    # Function starts and data values checked against the PAL XBE.
    for canonical, pal in ((0xC5A80, 0xC5A80), (0x114030, 0x114030),
                           (0x115B40, 0x115B40), (0x11B890, 0x11B8C0),
                           (0x13A920, 0x13A950), (0x20F6A0, 0x20F6D0),
                           (0x217FF0, 0x218010), (0x23F45C, 0x23F47C),
                           (0x26A420, 0x26A440), (0x27EDFC, 0x27EE1C),
                           (0x2888C8, 0x2888E8), (0x2AC290, 0x2AC2B0)):
        assert PAL.to_guest(canonical) == pal
        assert PAL.to_canonical(pal) == canonical
    # Outside the image nothing moves: heap, stacks and hardware.
    for va in (0x10000, 0x600000, 0xFE800000):
        assert PAL.to_guest(va) == va


def test_pal_insertion_window_fails_closed():
    for va in (0x115B41, 0x117000, 0x1184CF):
        try:
            PAL.to_guest(va)
        except ValueError:
            pass
        else:
            raise AssertionError("An address in the unresolved window must not be guessed")
    assert PAL.to_canonical(0x117000) is None
    assert PAL.to_canonical(0x118500) == 0x1184D0


def test_pal_canonical_names_round_trip_without_collisions():
    seen = {}
    for pal in range(0x115B00, 0x118600, 0x10):
        name = PAL.rename_symbols(f"sub_{pal:08X}")
        assert name not in seen, (name, seen.get(name), pal)
        seen[name] = pal
    assert PAL.rename_symbols("sub_00117000") == "sub_00117000_pal"
    text = ("void sub_0021C80F_gen(void);\n"
            "loc_00121B80: ;\n"
            "RECOMP_ABI_CALL(0x0021C80Fu, sub_0021C80F);\n"
            "{ 0x0021C80Fu, (recomp_func_t)sub_0021C80F },\n")
    # Names become canonical; literal guest values keep PAL addresses.
    assert PAL.rename_symbols(text) == (
        "void sub_0021C7EF_gen(void);\n"
        "loc_00121B50: ;\n"
        "RECOMP_ABI_CALL(0x0021C80Fu, sub_0021C7EF);\n"
        "{ 0x0021C80Fu, (recomp_func_t)sub_0021C7EF },\n")
    assert PAL.guest_symbols("void sub_0021C7EF(void) {}\nextern void sub_00121B50_gen(void);") == \
        "void sub_0021C80F(void) {}\nextern void sub_00121B80_gen(void);"


def test_marker_placeholders_keep_recompiler_format():
    template = "MEM32({0x562BD8}) = {0x121B50}; PUSH32(esp, {0x00102D67u}); MEM32(ebp + 0x5CB0)"
    assert NTSC.text(template) == "MEM32(0x562BD8) = 0x121B50; PUSH32(esp, 0x00102D67u); MEM32(ebp + 0x5CB0)"
    assert PAL.text(template) == "MEM32(0x562BF8) = 0x121B80; PUSH32(esp, 0x00102D67u); MEM32(ebp + 0x5CB0)"


def test_hooks_follow_the_selected_release():
    registration = "MEM32(0x562BF8) = 0x121B80;\nMEM32(0x562BC8) = 0x121BF0;"
    with patch.object(generate, "RELEASE", PAL):
        assert generate.scope_mask_boundary("sub_00121CB0", registration) == registration
        body = "loc_0016EA90: ;\n    xmm0.f[0] = MEMF(0x295CC8);\n"
        assert "black_osc_factor()" in generate.title_hooks("sub_0016EA90", body)
    try:
        generate.scope_mask_boundary("sub_00121CB0", registration)
    except ValueError:
        pass
    else:
        raise AssertionError("NTSC hooks must not accept PAL addresses")


def test_release_seeds_and_header():
    with TemporaryDirectory() as directory:
        directory = Path(directory)
        source = directory / "seeds.json"
        source.write_text(json.dumps([{"start": "0x0011B890", "flags": 2}]), encoding="utf-8")
        with patch.object(generate, "RELEASE", NTSC):
            assert generate.release_seeds(source, directory / "out.json") == source
        with patch.object(generate, "RELEASE", PAL):
            out = generate.release_seeds(source, directory / "out.json")
            assert json.loads(out.read_text(encoding="utf-8")) == [{"start": "0x0011B8C0", "flags": 2}]
            generate.write_release_header(directory / "black_release_gen.h")
        header = (directory / "black_release_gen.h").read_text(encoding="utf-8")
        assert "#define BLACK_RELEASE_PAL 1" in header
        assert "#define BLACK_POST_SHIFT 0x20u" in header
    assert "#define BLACK_RELEASE_PAL 0" in NTSC.header()
