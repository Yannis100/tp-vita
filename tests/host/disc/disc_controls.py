"""Negative controls for the first-boot diagnosis tests (tests/host/disc/run.sh).

Each control breaks one thing in boot_diag.hpp / boot_text.hpp / boot_font.hpp / vita_shader_dump.hpp and must make the test
fail (not just fail to compile). Files are restored and compared byte for byte. Run:  python3 tests/host/disc/disc_controls.py
Do not run it while the e2e harness is building: vita_shader_dump.hpp is also part of the merged Aurora tree.
"""
import filecmp, os, shutil, subprocess, sys, tempfile

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
files = {
    'diag': root + '/vita/src/boot_diag.hpp',
    'text': root + '/vita/src/boot_text.hpp',
    'font': root + '/vita/src/boot_font.hpp',
    'dump': root + '/vita/aurora/overlay/gfx/vita_shader_dump.hpp',
}
tmp = os.environ.get('TPV_CTL_TMP', tempfile.gettempdir() + '/tpv/ctl')
os.makedirs(tmp, exist_ok=True)
bak = {k: os.path.join(tmp, 'disc_bak_' + os.path.basename(v)) for k, v in files.items()}
for k in files:
    shutil.copy(files[k], bak[k])
controls = [
 ('rgb red/blue swapped',          'text', 'static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r', 'static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b'),
 ('glyph drawn mirrored',          'text', '(1u << (kGlyphW - 1 - gx))', '(1u << gx)'),
 ('wrap allows one extra column',  'text', 'const size_t width = cols - prefix;', 'const size_t width = cols - prefix + 1;'),
 ('layout marker miscounts',       'text', 'std::to_string(body.size() - static_cast<size_t>(shown))', 'std::to_string(body.size() - static_cast<size_t>(shown) + 1)'),
 ('layout drops the footer pad',   'text', 'const int room = rows - static_cast<int>(head.size()) - static_cast<int>(foot.size()) - 1;', 'const int room = rows - static_cast<int>(head.size()) - static_cast<int>(foot.size()) + 3;'),
 ('scan accepts only GZ2E01',      'diag', 'for (const char* accepted : {"GZ2E01", "GZ2J01", "GZ2P01"}) {\n        if (d.gameId', 'for (const char* accepted : {"GZ2E01"}) {\n        if (d.gameId'),
 ('RVZ magic not sniffed',         'diag', 'if (std::memcmp(head, "RVZ\\x01", 4) == 0) return "RVZ";', ''),
 ('JSON key/value not told apart', 'diag', "if (i >= text.size() || text[i] != ':') {\n            continue; // the text was a value, not a key\n        }", "if (i >= text.size() || text[i] != ':') {\n            return false;\n        }"),
 ('JSON escapes not decoded',      'diag', "default: value += text[i]; break; // \\\" \\\\ \\/", "default: value += '\\\\'; value += text[i]; break;"),
 ('boot log not flushed per line', 'diag', '            std::fflush(file_);\n', ''),
 ('boot log not rotated',          'diag', '        std::rename(path_.c_str(), previous.c_str());\n', ''),
 ('short-copy hint wrong number',  'diag', 'std::to_string(kGameCubeDiscBytes - size) +', 'std::to_string(size) +'),
 ('short-copy hint on a CISO',     'diag', '(ext == "ISO" || ext == "GCM")', '(ext == "ISO" || ext == "GCM" || ext == "CISO")'),
 ('shader dump label unsanitised', 'dump', "if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {\n      c = '_';\n    }", "if (false) {\n      c = '_';\n    }"),
 ('shader dump truncates source',  'dump', 'std::fwrite(source.data(), 1, source.size(), f) == source.size()', 'std::fwrite(source.data(), 1, source.size() / 2, f) == source.size() / 2'),
 ('font: Q drawn as O',            'font', '{0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101, 0b00000}, // Q', '{0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000}, // Q'),
 ('font: g loses its descender',   'font', '{0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b10001, 0b01110}, // g', '{0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b10001, 0b00000}, // g'),
 ('font: 0 and O identical',       'font', '{0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110, 0b00000}, // 0', '{0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000}, // 0'),
 ('decide: stale saved verdict used',  'diag', 'if (!in.saved.empty() && in.savedExists && in.savedCheck == DiscCheck::Ok) {', 'if (!in.saved.empty() && in.savedCheck == DiscCheck::Ok) {'),
 ('decide: found disc not injected',   'diag', '        d.inject = in.found;\n        return d;', '        return d;'),
 ('decide: saved named before found',  'diag', 'if (!in.found.empty()) {\n        d.screen = bad_disc_screen(in.found', 'if (!in.found.empty() && in.saved.empty()) {\n        d.screen = bad_disc_screen(in.found'),
 ('decide: rejected found counts ok',  'diag', 'if (!in.found.empty() && in.foundCheck == DiscCheck::Ok) {', 'if (!in.found.empty()) {'),
 ('decide: missing saved path hidden', 'diag', 'in.saved.empty() ? "" : "that file does not exist"', '""'),
]
bad = 0
for name, key, old, new in controls:
    src = open(bak[key]).read()
    if src.count(old) != 1:
        print(f'!! {name}: pattern occurs {src.count(old)} times, expected exactly once'); bad += 1; continue
    open(files[key], 'w').write(src.replace(old, new, 1))
    try:
        r = subprocess.run(['sh', root + '/tests/host/disc/run.sh'], capture_output=True, text=True, timeout=300)
    finally:
        shutil.copy(bak[key], files[key])
    out = r.stdout + r.stderr
    fails = [l for l in out.splitlines() if l.startswith('FAIL')]
    summary = [l for l in out.splitlines() if 'checks,' in l]
    compile_err = 'error:' in out
    status = 'FAILS (good)' if (r.returncode != 0 and not compile_err) else ('COMPILE ERROR' if compile_err else 'STILL PASSES (bad)')
    if status != 'FAILS (good)':
        bad += 1
    print(f'{status:20s} {name:34s} {len(fails):3d} failing line(s)  {summary[-1] if summary else ""}', flush=True)
for k in files:
    assert filecmp.cmp(bak[k], files[k], shallow=False), k
print(f'restored byte-for-byte; {len(controls)} control(s), unexpected results: {bad}')
sys.exit(1 if bad else 0)
