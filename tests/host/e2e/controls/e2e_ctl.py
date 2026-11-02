"""Negative-control runner for the e2e harness (v2).

A control = a mutation of the renderer (or of a file the harness compiles) + the exact set of scenes it must fail.
It runs the WHOLE suite and compares the failing set with the expected one: a control that fails nothing did not reach the
build, one that fails extra scenes is not specific.

Keys of a control dict:
  name     text
  fails    list of scene names that must fail (the harness' own "teardown" check counts as a scene)
  edits    list of (file, old, new): `file` is an overlay source (the tree builder copies it into build/aurora_lib);
           `old` must occur exactly once
  file/old/new   shorthand for a single edit
  upstream (relpath, old, new): an upstream Aurora lib file with no overlay copy; the mutated file is written to the
           overlay for the run and deleted afterwards
  mutate_copy (srcpath, makevar, dst, old, new): mutate a copy of a game file and point the Makefile variable at it
  asan     run the suite under AddressSanitizer (TPV_E2E_ASAN=1)
  crash    regex that must appear in the output of a run that is NOT a clean pass (ASan report, abort); with `crash`
           set the scene set is only compared if every scene printed
Files are always restored and compared byte for byte; a final clean run (plain build) must pass.
Malformed definitions (unknown key, wrong tuple length, missing file) are reported before anything runs.
"""
import filecmp, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
RUN = ['sh', ROOT + '/tests/host/e2e/run.sh']
TMP = os.environ.get('TPV_CTL_TMP', '/tmp/tpv/ctl')
BAK = os.path.join(TMP, 'e2e_bak')
OBJ_GF = os.path.join(os.environ.get('TPV_E2E_OBJ', '/tmp/tpv/e2e/obj'), 'game_GFPixel.o')
OVERLAY = ROOT + '/vita/aurora/overlay'
AURORA_DIR = os.environ.get('AURORA_DIR', os.path.join(ROOT, '..', 'dusklight', 'extern', 'aurora'))
AURORA_LIB = os.path.join(AURORA_DIR, 'lib')
DUSKLIGHT = os.path.abspath(os.path.join(AURORA_DIR, '..', '..'))  # the game sources
ENV = dict(os.environ)  # VITASDK must be set by the caller (run.sh checks it)


def run_suite(env_extra=None):
    env = dict(ENV, **(env_extra or {}))
    t = time.time()
    r = subprocess.run(RUN, capture_output=True, text=True, timeout=1800, env=env)
    out = r.stdout + r.stderr
    status = {}
    for line in out.splitlines():
        m = re.match(r'^(\S+)\s+(ok|FAIL)\s', line)
        if m:
            status[m.group(1)] = m.group(2)
    summary = [l for l in out.splitlines() if 'pixel checks' in l]
    build_err = bool(re.search(r'error:|Error 1|\*\*\*', out)) and not status
    return dict(status=status, build_err=build_err, summary=(summary[-1] if summary else ''), secs=time.time() - t,
                out=out, rc=r.returncode)


def normalise(ctl):
    edits = list(ctl.get('edits', []))
    if ctl.get('file'):
        edits.append((ctl['file'], ctl['old'], ctl['new']))
    return edits


KEYS = {'name', 'fails', 'file', 'old', 'new', 'edits', 'upstream', 'mutate_copy', 'asan', 'crash'}


def validate(controls):
    """Shape errors in a control list, found before anything is mutated or run (a malformed control used to crash the
    group halfway: a 3-item mutate_copy against the 5 items the runner unpacks)."""
    errors, seen = [], set()
    for i, ctl in enumerate(controls):
        name = ctl.get('name', f'<control #{i}>')
        where = f'{name!r}'
        if name in seen:
            errors.append(f'{where}: duplicate name')
        seen.add(name)
        for k in sorted(set(ctl) - KEYS):
            errors.append(f'{where}: unknown key {k!r}')
        if not isinstance(ctl.get('fails'), (list, tuple, set)):
            errors.append(f'{where}: "fails" must be a list of scene names')
        single = [k for k in ('file', 'old', 'new') if k in ctl]
        if single and len(single) != 3:
            errors.append(f'{where}: file/old/new must be given together (got {single})')
        for e in ctl.get('edits', []):
            if not (isinstance(e, (list, tuple)) and len(e) == 3):
                errors.append(f'{where}: each "edits" entry is (file, old, new)')
        for k, n in (('upstream', 3), ('mutate_copy', 5)):
            if k in ctl and not (isinstance(ctl[k], (list, tuple)) and len(ctl[k]) == n):
                errors.append(f'{where}: {k!r} must be a {n}-tuple, got {ctl[k]!r:.60}')
        if not any(k in ctl for k in ('file', 'edits', 'upstream', 'mutate_copy')):
            errors.append(f'{where}: nothing to mutate (file/edits/upstream/mutate_copy)')
        paths = [e[0] for e in ctl.get('edits', []) if isinstance(e, (list, tuple)) and e]
        if 'file' in ctl:
            paths.append(ctl['file'])
        if isinstance(ctl.get('mutate_copy'), (list, tuple)) and ctl['mutate_copy']:
            paths.append(ctl['mutate_copy'][0])
        for path in paths:
            if not os.path.isfile(path):
                errors.append(f'{where}: source file not found: {path}')
        if isinstance(ctl.get('upstream'), (list, tuple)) and len(ctl['upstream']) == 3:
            rel = ctl['upstream'][0]
            if not os.path.isfile(os.path.join(AURORA_LIB, rel)):
                errors.append(f'{where}: upstream file not found: {rel}')
    return errors


def run_controls(controls, only=None):
    errors = validate(controls)
    if errors:
        print('malformed control definitions, nothing was run:')
        for e in errors:
            print('  ' + e)
        return len(errors)
    os.makedirs(BAK, exist_ok=True)
    pristine = {}
    problems = 0
    for ctl in controls:
        name = ctl['name']
        if only and not any(o in name for o in only):
            continue
        expected = set(ctl['fails'])
        env_extra = {'TPV_E2E_ASAN': '1'} if ctl.get('asan') else {}
        touched, added, bad = [], [], False
        try:
            by_file = {}
            for path, old, new in normalise(ctl):
                by_file.setdefault(path, []).append((old, new))
            for path, edits in by_file.items():
                if path not in pristine:
                    pristine[path] = os.path.join(BAK, path.replace('/', '_'))
                    shutil.copy(path, pristine[path])
                text = open(pristine[path]).read()
                for old, new in edits:
                    if text.count(old) != 1:
                        print(f'!! {name}: pattern found {text.count(old)} times (want exactly 1): {old[:70]!r}')
                        bad = True
                        break
                    text = text.replace(old, new, 1)
                if bad:
                    break
                open(path, 'w').write(text)
                touched.append(path)
            if not bad and ctl.get('upstream'):
                rel, old, new = ctl['upstream']
                dst = os.path.join(OVERLAY, rel)
                assert not os.path.exists(dst), f'{dst} already in the overlay'
                text = open(os.path.join(AURORA_LIB, rel)).read()
                if text.count(old) != 1:
                    print(f'!! {name}: upstream pattern found {text.count(old)} times (want exactly 1): {old[:70]!r}')
                    bad = True
                else:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, 'w').write(text.replace(old, new, 1))
                    added.append(dst)
            if not bad and ctl.get('mutate_copy'):
                srcpath, var, dst, old, new = ctl['mutate_copy']
                text = open(srcpath).read()
                if text.count(old) != 1:
                    print(f'!! {name}: pattern found {text.count(old)} times (want exactly 1)')
                    bad = True
                else:
                    open(dst, 'w').write(text.replace(old, new, 1))
                    env_extra[var] = dst
            if bad:
                problems += 1
                continue
            res = run_suite(env_extra)
        finally:
            for path in touched:
                shutil.copy(pristine[path], path)
            for dst in added:
                os.remove(dst)
                d = os.path.dirname(dst)
                while d != OVERLAY and not os.listdir(d):
                    os.rmdir(d)
                    d = os.path.dirname(d)
            if ctl.get('mutate_copy') and os.path.exists(OBJ_GF):
                os.remove(OBJ_GF)  # built from the mutated copy; never let make reuse it
        status, out = res['status'], res['out']
        failing = {k for k, v in status.items() if v == 'FAIL'}
        crashed = res['rc'] != 0 and not failing
        crash_re = ctl.get('crash')
        if res['build_err']:
            verdict = 'COMPILE/RUN ERROR'
        elif crash_re:
            if re.search(crash_re, out) and res['rc'] != 0:
                verdict = 'CRASHES as expected' if failing == expected else 'crashes, scene set differs'
            else:
                verdict = 'NO CRASH (bad)' if res['rc'] == 0 else 'crashes, wrong report'
        elif crashed:
            verdict = 'CRASH/ABORT (bad)'
        elif failing == expected:
            verdict = 'FAILS as expected'
        elif not failing:
            verdict = 'STILL PASSES (bad)'
        elif expected <= failing:
            verdict = 'fails, but too broad'
        else:
            verdict = 'fails the WRONG scenes'
        ok = verdict in ('FAILS as expected', 'CRASHES as expected')
        problems += not ok
        print(f'{verdict:22s} {name}  [{res["secs"]:.0f}s]')
        print(f'{"":22s} failing: {sorted(failing)}')
        if not ok:
            print(f'{"":22s} expected: {sorted(expected)}  missing: {sorted(expected - failing)}  extra: {sorted(failing - expected)}')
            print(f'{"":22s} rc={res["rc"]}')
            print(out[-1800:] if (res['build_err'] or crash_re or crashed) else '')
        sys.stdout.flush()
    for path, bak in pristine.items():
        assert filecmp.cmp(bak, path, shallow=False), f'{path} not restored'
    res = run_suite()
    clean = not res['build_err'] and res['status'] and res['rc'] == 0 and all(v == 'ok' for v in res['status'].values())
    print(f'restored byte-for-byte; clean run: {res["summary"]}')
    if not clean:
        problems += 1
        print(res['out'][-1500:])
    print(f'{len(controls) if not only else "selected"} control(s), {problems} problem(s)')
    return problems
