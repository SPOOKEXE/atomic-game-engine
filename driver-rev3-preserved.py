#!/usr/bin/env python3
"""Root-only incremental diagnostic/candidate driver for the v10 Group/IO investigation."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = Path('/home/declan/Documents/GitHub/atomic-game-engine')
V9_ROOT = Path('/tmp/color-depth-group-io-gradient-invert-finite-gate-2026-10-01-v9-full-fresh')
V9_EVIDENCE = REPO / '.cache/build/dev/evidence/group-io-gradient-invert-finite-2026-10-01-v9-full-fresh/root_review'
OLD_SOURCE = str(V9_ROOT / 'source')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def load_plan(kind):
    if kind not in ('diagnostic', 'candidate'):
        raise ValueError('kind must be diagnostic or candidate')
    return json.loads((ROOT / f'{kind}-plan-rev3.json').read_text())


def proof():
    return json.loads((ROOT / 'freeze-proof-rev3.json').read_text())


def verify(kind):
    plan = load_plan(kind)
    frozen = proof()
    if digest(ROOT / 'driver.py') != frozen['driver_sha256']:
        raise RuntimeError('driver hash differs from frozen proof')
    for plan_kind in ('diagnostic', 'candidate'):
        path = ROOT / f'{plan_kind}-plan-rev3.json'
        expected = frozen['plan_sha256'][plan_kind]
        if digest(path) != expected:
            raise RuntimeError(f'{plan_kind} plan hash differs from frozen proof')
        stored = json.loads(path.read_text())
        if stored.get('driver_sha256') != frozen['driver_sha256']:
            raise RuntimeError(f'{plan_kind} plan driver pin mismatch')
    if len(plan['input_pins']) != 1804 or plan['input_pin_count'] != 1804:
        raise RuntimeError('expected exactly 1,804 frozen source/dependency pins')
    pins = {}
    for item in plan['input_pins']:
        path = Path(item['path'])
        if not path.is_file() or digest(path) != item['sha256']:
            raise RuntimeError('input pin mismatch: ' + str(path))
        pins[str(path.resolve())] = item['sha256']
    if len(plan['frozen_v9_objects']) != 140 or plan['frozen_v9_object_count'] != 140:
        raise RuntimeError('expected all 140 v9 full-fresh object pins')
    for item in plan['frozen_v9_objects']:
        path = Path(item['path'])
        if not path.is_file() or digest(path) != item['sha256']:
            raise RuntimeError('v9 object pin mismatch: ' + str(path))
    for name, item in plan['fresh_v9_archives'].items():
        path = Path(item['path'])
        if not path.is_file() or digest(path) != item['sha256']:
            raise RuntimeError('v9 archive pin mismatch: ' + str(path))
        members = subprocess.check_output(['/usr/bin/ar', 't', str(path)], text=True).splitlines()
        if members != item['members']:
            raise RuntimeError('v9 archive member order mismatch: ' + str(path))
    v9_plan = json.loads((V9_ROOT / 'plan.json').read_text())
    v9_units = {unit['relative_source']: unit for unit in v9_plan['units']}
    changed = {entry['relative_source']: entry for entry in plan['changed_translation_units']}
    for rel, item in changed.items():
        unit = v9_units[rel]
        src = Path(item['source'])
        if item['source'] != str(Path(plan['source_mirror']) / rel):
            raise RuntimeError('source path escaped frozen mirror: ' + item['source'])
        if digest(src) != item['source_sha256']:
            raise RuntimeError('changed source hash mismatch: ' + item['source'])
        if item['object'] != str(Path(plan['output']) / 'objects' / (rel + '.o')):
            raise RuntimeError('changed object path/stem mismatch: ' + item['object'])
        if len(item['argv']) == 0 or item['argv'][item['argv'].index('-c') + 1] != item['source']:
            raise RuntimeError('compile argv source mismatch: ' + rel)
        if item['argv'][item['argv'].index('-o') + 1] != item['object']:
            raise RuntimeError('compile argv object mismatch: ' + rel)
        if item['argv'][item['argv'].index('-MF') + 1] != item['object'] + '.d':
            raise RuntimeError('compile argv dependency path mismatch: ' + rel)
        if unit['relative_source'] != rel:
            raise RuntimeError('source unit mapping mismatch')
    expected_changes = 2 if kind == 'diagnostic' else 4
    if len(changed) != expected_changes:
        raise RuntimeError('unexpected incremental compile count')
    for command in plan['test_commands']:
        if 'exit' in command or 'compiler_exit' in command or 'seconds' in command:
            raise RuntimeError('future test command contains inherited result metadata')
        if Path(command['argv'][0]).parent != Path(plan['output']) / 'tests':
            raise RuntimeError('test command binary path mismatch')
    return plan, pins, v9_units


def dependency_paths(depfile):
    raw = Path(depfile).read_text(errors='backslashreplace').replace('\\\n', ' ')
    if ':' not in raw:
        raise RuntimeError('compiler dependency file has no target separator: ' + str(depfile))
    return shlex.split(raw.split(':', 1)[1])


def command(item, out, name):
    start = time.monotonic()
    stem = ''.join(c if c.isalnum() or c in '._-' else '_' for c in name)
    stdout = out / 'logs' / f'{stem}.stdout'
    stderr = out / 'logs' / f'{stem}.stderr'
    env = dict(os.environ, CCACHE_DISABLE='1', TMPDIR=str(out / 'tmp'))
    with stdout.open('wb') as so, stderr.open('wb') as se:
        proc = subprocess.run(item['argv'], cwd=item.get('cwd', str(REPO)), env=env, stdout=so, stderr=se)
    result = {'name': name, 'argv': item['argv'], 'cwd': item.get('cwd', str(REPO)),
              'exit': proc.returncode, 'seconds': time.monotonic() - start,
              'stdout': str(stdout), 'stderr': str(stderr),
              'stdout_sha256': digest(stdout), 'stderr_sha256': digest(stderr)}
    return result


def link_argv(stage, plan, changed, out):
    old_default = str(V9_ROOT / 'default')
    args = []
    for arg in stage['argv']:
        if arg.startswith(old_default + '/objects/'):
            rel_obj = arg[len(old_default + '/objects/'):]
            rel = rel_obj[:-2] if rel_obj.endswith('.o') else rel_obj
            if rel in changed:
                args.append(str(out / 'objects' / rel_obj))
            else:
                args.append(str(V9_EVIDENCE / 'objects' / rel_obj))
        elif arg.startswith(old_default + '/lib/'):
            args.append(str(out / 'lib' / Path(arg).name))
        elif arg.startswith(old_default + '/tests/'):
            args.append(str(out / 'tests' / Path(arg).name))
        else:
            args.append(arg)
    return args


def build(kind):
    plan, pins, v9_units = verify(kind)
    out = Path(plan['output'])
    if out.exists():
        raise RuntimeError('output exists; refusing to overwrite: ' + str(out))
    for directory in ('objects', 'lib', 'tests', 'logs', 'tmp'):
        (out / directory).mkdir(parents=True, exist_ok=True)
    changed = {item['relative_source']: item for item in plan['changed_translation_units']}
    for item in plan['frozen_v9_objects']:
        if item['relative_source'] in changed:
            continue
        target = out / 'objects' / (item['relative_source'] + '.o')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(Path(item['path']))
    for item in plan['fresh_v9_archives'].values():
        shutil.copy2(item['path'], out / 'lib' / Path(item['path']).name)
    archive_results = []
    for name, item in plan['fresh_v9_archives'].items():
        copied = out / 'lib' / name
        archive_results.append({'name': name, 'action': 'copied-from-v9-full-fresh',
                                'exit': 0, 'sha256': digest(copied), 'members': item['members']})
    (out / 'archive-results.json').write_text(json.dumps(archive_results, indent=2) + '\n')
    compile_results = []
    for entry in plan['changed_translation_units']:
        item = v9_units[entry['relative_source']]
        argv = list(entry['argv'])
        argv = [arg.replace(OLD_SOURCE, plan['source_mirror']) if isinstance(arg, str) else arg for arg in argv]
        argv[argv.index('-c') + 1] = entry['source']
        argv[argv.index('-o') + 1] = entry['object']
        argv[argv.index('-MF') + 1] = entry['object'] + '.d'
        result = command({'argv': argv, 'cwd': item['cwd']}, out, 'compile_' + entry['relative_source'])
        result['compiler_exit'] = result['exit']
        result['dependency_audit_exit'] = None
        result['dependency_error'] = None
        if result['compiler_exit'] != 0:
            compile_results.append(result)
            (out / 'compile-results.json').write_text(json.dumps(compile_results, indent=2) + '\n')
            (out / 'build-results.json').write_text(json.dumps({
                'success': False, 'phase': 'compile', 'compiler_failures': 1,
                'dependency_audit_failures': 0, 'compiler_exit': result['compiler_exit'],
                'dependency_error': None
            }, indent=2) + '\n')
            return 1
        try:
            depfile = Path(entry['object'] + '.d')
            deps = []
            for raw in dependency_paths(depfile):
                path = Path(raw)
                key = str(path.resolve())
                if key not in pins:
                    raise RuntimeError('actual dependency is not pinned in the 1,804-file closure: ' + raw)
                deps.append({'path': key, 'sha256': pins[key]})
            result['dependencies'] = deps
            result['dependency_audit_exit'] = 0
        except Exception as exc:
            result['dependency_audit_exit'] = 1
            result['dependency_error'] = str(exc)
            compile_results.append(result)
            (out / 'compile-results.json').write_text(json.dumps(compile_results, indent=2) + '\n')
            (out / 'build-results.json').write_text(json.dumps({
                'success': False, 'phase': 'dependency_audit', 'compiler_failures': 0,
                'dependency_audit_failures': 1, 'compiler_exit': result['compiler_exit'],
                'dependency_error': result['dependency_error']
            }, indent=2) + '\n')
            return 1
        compile_results.append(result)
        (out / 'compile-results.json').write_text(json.dumps(compile_results, indent=2) + '\n')
    (out / 'compile-results.json').write_text(json.dumps(compile_results, indent=2) + '\n')
    if kind == 'candidate':
        archive = out / 'lib/libengine_imagegraphio.a'
        pxcx_obj = out / 'objects/mono.engine/imagegraphio/src/PxcxEdit.cpp.o'
        archive_commands = []
        for argv in (['/usr/bin/ar', 'r', str(archive), str(pxcx_obj)],
                     ['/usr/bin/ranlib', str(archive)]):
            proc = subprocess.run(argv, cwd=str(REPO), capture_output=True)
            archive_commands.append({'argv': argv, 'exit': proc.returncode,
                                     'stdout': proc.stdout.decode('utf-8', 'backslashreplace'),
                                     'stderr': proc.stderr.decode('utf-8', 'backslashreplace')})
            if proc.returncode:
                archive_results.append({'name': 'libengine_imagegraphio.a', 'action': 'replace-member',
                                        'commands': archive_commands, 'exit': proc.returncode})
                (out / 'archive-results.json').write_text(json.dumps(archive_results, indent=2) + '\n')
                return proc.returncode
        members = subprocess.check_output(['/usr/bin/ar', 't', str(archive)], text=True).splitlines()
        expected_members = plan['fresh_v9_archives']['libengine_imagegraphio.a']['members']
        if members != expected_members or members != ['PxcxEdit.cpp.o', 'PxcxImport.cpp.o']:
            raise RuntimeError('candidate archive member order or membership changed')
        old_import = subprocess.check_output(['/usr/bin/ar', 'p', plan['fresh_v9_archives']['libengine_imagegraphio.a']['path'], 'PxcxImport.cpp.o'])
        new_import = subprocess.check_output(['/usr/bin/ar', 'p', str(archive), 'PxcxImport.cpp.o'])
        if old_import != new_import:
            raise RuntimeError('untouched PxcxImport archive member changed')
        new_edit = subprocess.check_output(['/usr/bin/ar', 'p', str(archive), 'PxcxEdit.cpp.o'])
        if hashlib.sha256(new_edit).hexdigest() != digest(pxcx_obj):
            raise RuntimeError('candidate PxcxEdit archive member differs from compiled object')
        archive_results.append({'name': 'libengine_imagegraphio.a', 'action': 'replace-member',
                                'commands': archive_commands, 'exit': 0, 'members': members,
                                'sha256': digest(archive), 'preserved_import_member_sha256': hashlib.sha256(new_import).hexdigest(),
                                'candidate_edit_member_sha256': hashlib.sha256(new_edit).hexdigest()})
        (out / 'archive-results.json').write_text(json.dumps(archive_results, indent=2) + '\n')
    link_results = []
    v9_plan = json.loads((V9_ROOT / 'plan.json').read_text())
    for stage in v9_plan['stages']:
        if not stage['name'].startswith('link_'):
            continue
        item = {'name': stage['name'], 'argv': link_argv(stage, plan, changed, out), 'cwd': stage['cwd']}
        result = command(item, out, stage['name'])
        link_results.append(result)
        (out / 'link-results.json').write_text(json.dumps(link_results, indent=2) + '\n')
        if result['exit'] != 0:
            (out / 'build-results.json').write_text(json.dumps({'success': False, 'phase': 'link'}, indent=2) + '\n')
            return result['exit']
    binaries = {Path(test['argv'][0]).name: digest(test['argv'][0]) for test in plan['test_commands']}
    build_record = {'success': True, 'compiled_incremental': len(changed),
                    'reused_v9_objects': 140 - len(changed),
                    'source_mirror': plan['source_mirror'], 'binary_sha256': binaries,
                    'graph_archive_sha256': digest(out / 'lib/libengine_imagegraph.a'),
                    'graphio_archive_sha256': digest(out / 'lib/libengine_imagegraphio.a')}
    (out / 'build-results.json').write_text(json.dumps(build_record, indent=2) + '\n')
    return 0


def test(kind):
    plan, _, _ = verify(kind)
    out = Path(plan['output'])
    built_path = out / 'build-results.json'
    if not built_path.is_file() or not json.loads(built_path.read_text()).get('success'):
        raise RuntimeError('tests require a successful isolated incremental build')
    results = []
    for item in plan['test_commands']:
        if digest(item['argv'][0]) != json.loads(built_path.read_text())['binary_sha256'][Path(item['argv'][0]).name]:
            raise RuntimeError('test binary hash changed: ' + item['argv'][0])
        result = command(item, out, 'test_' + item['name'])
        results.append(result)
        (out / 'test-results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(item['exit'] != 0 for item in results))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('verify', 'build', 'test'))
    parser.add_argument('kind', choices=('diagnostic', 'candidate'))
    args = parser.parse_args()
    if args.action == 'verify':
        plan, _, _ = verify(args.kind)
        print(json.dumps({'verified_inputs': len(plan['input_pins']),
                          'verified_v9_objects': len(plan['frozen_v9_objects']),
                          'changed_compile_units': len(plan['changed_translation_units']),
                          'output': plan['output']}))
        return 0
    if args.action == 'build':
        return build(args.kind)
    return test(args.kind)

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f'incremental driver failure: {exc}', file=sys.stderr)
        raise SystemExit(2)
