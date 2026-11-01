#!/usr/bin/env python3
"""Prepare, build and check FastTT against the pinned official Ceph release."""
import argparse
import hashlib
import json
from math import comb
from pathlib import Path
import platform
import subprocess
import sys
from datetime import datetime, timezone

TSX = Path(__file__).resolve().parents[1]
LOCK = json.loads((TSX / 'repro/versions.json').read_text())
TARGETS = ['ec_tsx', 'ec_tsx2', 'ec_tsx4', 'ec_isa', 'ec_jerasure',
           'ceph_erasure_code_benchmark']


def run(args, cwd=None, capture=False):
    args = [str(a) for a in args]
    if not capture:
        print('+', ' '.join(args), flush=True)
    try:
        return subprocess.run(args, cwd=cwd, check=True, text=True,
                              stdout=subprocess.PIPE if capture else None,
                              stderr=subprocess.STDOUT if capture else None).stdout
    except subprocess.CalledProcessError as error:
        if capture and error.stdout:
            print(error.stdout, file=sys.stderr)
        raise


def git(root, *args):
    return run(['git', '-C', root, *args], capture=True).strip()


def check_sources(ceph):
    if git(ceph, 'rev-parse', 'HEAD') != LOCK['ceph']['commit']:
        raise RuntimeError('Ceph HEAD must be exactly ' + LOCK['ceph']['commit'])
    if TSX != (ceph / 'src/erasure-code/tsx').resolve():
        raise RuntimeError('Clone this TSX repository into CEPH/src/erasure-code/tsx')
    run(['git', '-C', TSX, 'merge-base', '--is-ancestor',
         LOCK['tsx']['algorithm_base'], 'HEAD'], capture=True)
    status = git(ceph, 'submodule', 'status', '--recursive')
    if any(line.startswith(('-', '+', 'U')) for line in status.splitlines()):
        raise RuntimeError('Run git submodule update --init --recursive first')


def prepare(ceph):
    check_sources(ceph)
    patches = sorted((TSX / 'repro/patches').glob('*.patch'))
    if not patches:
        raise RuntimeError('No Ceph integration patches found in repro/patches')
    for patch in patches:
        cmd = ['git', '-C', ceph, 'apply']
        if subprocess.run([str(x) for x in cmd + ['--reverse', '--check', patch]],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            print('Already applied:', patch.name)
        else:
            run(cmd + ['--check', patch])
            run(cmd + [patch])


def read_cache(build):
    cache = build / 'CMakeCache.txt'
    values = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(('#', '//')) or '=' not in line:
                continue
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    return values


def check_cache(ceph, build):
    """Allow retries of a failed configure, but never adopt another source/cache."""
    marker = build / 'fasttt-configure.json'
    if marker.exists():
        state = json.loads(marker.read_text())
        if (state.get('ceph_commit') != LOCK['ceph']['commit'] or
                state.get('ceph_root', str(ceph)) != str(ceph)):
            raise RuntimeError('Build marker belongs to another Ceph source; choose an empty --build-dir')
    cache = read_cache(build)
    if not (build / 'CMakeCache.txt').exists():
        return cache
    source = cache.get('CMAKE_HOME_DIRECTORY')
    if (not source or Path(source).resolve() != ceph or
            cache.get('CMAKE_PROJECT_NAME') != 'ceph' or
            cache.get('CMAKE_PROJECT_VERSION') != LOCK['ceph']['tag'].removeprefix('v') or
            cache.get('CMAKE_GENERATOR') != 'Ninja'):
        raise RuntimeError('Build cache must match this Ceph source, version and Ninja generator; choose an empty --build-dir')
    for key, value in cache.items():
        if key.startswith(('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS')):
            if '-mavx512' in value or '-march=native' in value:
                raise RuntimeError(f'Refusing experimental compiler flags in {key}; choose an empty --build-dir')
    return cache


def configure(ceph, build, jobs):
    prepare(ceph)
    flags = [
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_COMPILER=gcc-11',
        '-DCMAKE_CXX_COMPILER=g++-11', '-DCMAKE_C_FLAGS=', '-DCMAKE_CXX_FLAGS=',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DWITH_PYTHON3=3.10', '-DWITH_TESTS=ON', '-DWITH_EC_ISA_PLUGIN=ON',
        '-DWITH_TSX_CERASURE=OFF', '-DWITH_SYSTEM_BOOST=OFF', f'-DBOOST_J={jobs}',
        '-DWITH_MANPAGE=OFF', '-DWITH_MGR=OFF', '-DWITH_MGR_DASHBOARD_FRONTEND=OFF',
        '-DWITH_RADOSGW=OFF', '-DWITH_RBD=OFF', '-DWITH_CEPHFS=OFF',
        '-DWITH_LIBCEPHFS=OFF', '-DWITH_LIBCEPHSQLITE=OFF', '-DWITH_KRBD=OFF',
        '-DWITH_RDMA=OFF', '-DWITH_FUSE=OFF', '-DWITH_JAEGER=OFF',
        '-DWITH_LTTNG=OFF', '-DWITH_BABELTRACE=OFF', '-DWITH_OPENLDAP=OFF',
        '-DWITH_SYSTEMD=OFF', '-DWITH_LIBURING=OFF', '-DWITH_BLUESTORE=ON',
        '-DWITH_CCACHE=OFF'
    ]
    check_cache(ceph, build)
    build.mkdir(parents=True, exist_ok=True)
    marker = build / 'fasttt-configure.json'
    state = {'ceph_commit': LOCK['ceph']['commit'], 'ceph_root': str(ceph),
             'flags': flags, 'status': 'configuring'}
    marker.write_text(json.dumps(state, indent=2) + '\n')
    try:
        run(['cmake', '-S', ceph, '-B', build, '-G', 'Ninja', *flags])
    except (OSError, subprocess.CalledProcessError) as error:
        state.update(status='failed', error=str(error))
        marker.write_text(json.dumps(state, indent=2) + '\n')
        raise
    state['status'] = 'configured'
    marker.write_text(json.dumps(state, indent=2) + '\n')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def artifacts(build):
    return {target: build / ('bin' if target.endswith('benchmark') else 'lib') / (
        target if target.endswith('benchmark') else f'lib{target}.so')
        for target in TARGETS}


def source_hashes(ceph):
    """Fingerprint build inputs independently of unrelated documentation edits."""
    sources = (sorted(TSX.glob('*.cc')) + sorted(TSX.glob('*.h')) +
               sorted((TSX / 'repro/patches').glob('*.patch')) + [TSX / 'CMakeLists.txt'])
    hashes = {'tsx/' + str(p.relative_to(TSX)): sha256(p) for p in sources}
    for relative in ('src/erasure-code/CMakeLists.txt',
                     'src/test/erasure-code/ceph_erasure_code_benchmark.cc',
                     'src/test/erasure-code/ceph_erasure_code_benchmark.h',
                     'cmake/modules/BuildBoost.cmake'):
        hashes['ceph/' + relative] = sha256(ceph / relative)
    for relative in git(ceph, 'diff', '--name-only', 'HEAD').splitlines():
        source = ceph / relative
        if source.name == 'CMakeLists.txt' or source.suffix in ('.c', '.cc', '.cpp', '.cxx',
                                                               '.h', '.hpp', '.cmake', '.in'):
            hashes['ceph/' + relative] = sha256(source) if source.exists() else None
    return hashes


def report(ceph, build, name, results=None, status='observed'):
    build.mkdir(parents=True, exist_ok=True)
    files = sorted((TSX / 'repro/patches').glob('*.patch')) + [
        TSX / 'CMakeLists.txt', TSX / 'repro/versions.json', TSX / 'repro/reproduce.py']
    data = {'time_utc': datetime.now(timezone.utc).isoformat(), 'lock': LOCK,
            'status': status,
            'ceph_head': git(ceph, 'rev-parse', 'HEAD'),
            'tsx_head': git(TSX, 'rev-parse', 'HEAD'),
            'tsx_branch': git(TSX, 'branch', '--show-current'),
            'ceph_status': git(ceph, 'status', '--short'),
            'tsx_status': git(TSX, 'status', '--short'),
            'ceph_diff': git(ceph, 'diff', '--binary'),
            'tsx_diff': git(TSX, 'diff', '--binary'),
            'submodules': git(ceph, 'submodule', 'status', '--recursive'),
            'machine': platform.uname()._asdict(),
            'source_hashes': source_hashes(ceph),
            'hashes': {str(p.relative_to(TSX)): sha256(p) for p in files},
            'results': results}
    cache = read_cache(build)
    data['cmake_cache'] = {key: value for key, value in cache.items()
                           if key.startswith(('CMAKE_C_', 'CMAKE_CXX_', 'OpenMP_', 'HIREDIS_')) or
                           key in ('CMAKE_BUILD_TYPE', 'CMAKE_HOME_DIRECTORY',
                                   'CMAKE_PROJECT_VERSION', 'WITH_TSX_CERASURE', 'WITH_SYSTEM_BOOST')}
    marker = build / 'fasttt-configure.json'
    data['configure'] = json.loads(marker.read_text()) if marker.exists() else None
    commands = build / 'compile_commands.json'
    if commands.exists():
        selected = {'ErasureCodeTsx.cc', 'ErasureCodeTsx2.cc', 'ErasureCodeTsx4.cc',
                    'ceph_erasure_code_benchmark.cc'}
        data['compile_commands'] = [command for command in json.loads(commands.read_text())
                                    if Path(command['file']).name in selected]
    for tool in (['g++-11', '--version'], ['cmake', '--version'],
                 ['redis-server', '--version'], ['lscpu'],
                 ['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\n']):
        try:
            data[tool[0]] = run(tool, capture=True)
        except (OSError, subprocess.CalledProcessError) as error:
            data[tool[0]] = str(error)
    for artifact in artifacts(build).values():
        if artifact.exists():
            data['hashes'][str(artifact.relative_to(build))] = sha256(artifact)
    output = build / name
    output.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')
    print('Provenance:', output)


def check_build(ceph, build):
    check_cache(ceph, build)
    marker = build / 'fasttt-configure.json'
    if not marker.exists() or json.loads(marker.read_text()).get('status') != 'configured':
        raise RuntimeError('Run the build action after a successful configuration first')
    record = build / 'fasttt-build.json'
    if not record.exists():
        raise RuntimeError('Run the build action first to create trusted artifact provenance')
    data = json.loads(record.read_text())
    if (data.get('status') != 'passed' or
            data.get('ceph_head') != LOCK['ceph']['commit'] or
            data.get('source_hashes') != source_hashes(ceph)):
        raise RuntimeError('Sources differ from the last successful build; run the build action again')
    for artifact in artifacts(build).values():
        key = str(artifact.relative_to(build))
        if not artifact.exists() or data.get('hashes', {}).get(key) != sha256(artifact):
            raise RuntimeError(f'Artifact missing or changed since build: {key}; run the build action again')


def verify(ceph, build):
    """Compare recovered main-shard bytes for every 1..m erasure combination."""
    prepare(ceph)
    results = []
    try:
        check_build(ceph, build)
    except (RuntimeError, OSError, ValueError) as error:
        report(ceph, build, 'fasttt-verification.json',
               [{'stage': 'preflight', 'passed': False, 'error': str(error)}], status='failed')
        raise
    for k, m, plugin in [(6, 2, 'tsx2'), (6, 3, 'tsx'), (8, 3, 'tsx'), (10, 4, 'tsx4')]:
        # Aligned 64 KiB data chunks and paper-sized 2 MiB data chunks.
        for size in (k * 65536, k * 2097152):
            for pattern in ('constant', 'random'):
                for erased in range(1, m + 1):
                    cmd = [build / 'bin/ceph_erasure_code_benchmark',
                           '--erasure-code-dir', build / 'lib', '-p', plugin,
                           '-P', f'k={k}', '-P', f'm={m}', '-s', str(size), '-i', '1',
                           '-w', 'decode', '-E', 'exhaustive', '-e', str(erased), '-v',
                           '--input-pattern', pattern, '--input-seed', '1']
                    try:
                        completed = subprocess.run([str(x) for x in cmd], text=True,
                                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    except OSError as error:
                        results.append({'command': [str(x) for x in cmd], 'passed': False,
                                        'exit': None, 'error': str(error)})
                        report(ceph, build, 'fasttt-verification.json', results, status='failed')
                        raise
                    expected = comb(k + m, erased)
                    attempted = sum(line.startswith('chunks ') for line in completed.stdout.splitlines())
                    passed = completed.returncode == 0 and attempted == expected
                    result = {'command': [str(x) for x in cmd], 'exit': completed.returncode,
                              'output': completed.stdout, 'passed': passed,
                              'expected_erasure_combinations': expected,
                              'attempted_erasure_combinations': attempted,
                              'coverage_complete': attempted == expected}
                    results.append(result)
                    print(plugin, k, m, size, pattern, erased,
                          f'{attempted}/{expected}', 'PASS' if passed else 'FAIL', flush=True)
                    # Keep failures and continue so the baseline's limits remain visible.
    passed = all(result['passed'] for result in results)
    report(ceph, build, 'fasttt-verification.json', results, status='passed' if passed else 'failed')
    if not passed:
        raise RuntimeError('Correctness verification failed; see fasttt-verification.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'configure', 'build', 'verify', 'doctor'])
    parser.add_argument('--ceph-root', type=Path, default=TSX.parents[2])
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    ceph = args.ceph_root.resolve()
    build = (args.build_dir or ceph / 'build').resolve()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    check_sources(ceph)
    if args.action == 'prepare':
        prepare(ceph)
    elif args.action == 'configure':
        configure(ceph, build, args.jobs)
    elif args.action == 'build':
        try:
            configure(ceph, build, args.jobs)
            run(['cmake', '--build', build, '--parallel', args.jobs, '--target', *TARGETS])
        except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
            report(ceph, build, 'fasttt-build.json', {'error': str(error)}, status='failed')
            raise
        report(ceph, build, 'fasttt-build.json', status='passed')
    elif args.action == 'verify':
        verify(ceph, build)
    else:
        report(ceph, build, 'fasttt-environment.json')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as exc:
        print('ERROR:', exc, file=sys.stderr)
        sys.exit(1)
