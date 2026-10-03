#!/usr/bin/env python3
"""Project Win32/MSVC qualification; outputs are evidence, never deployment."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'qualification-output'
OUT.mkdir(exist_ok=True)
results = []

def run(name, args):
    result = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace')
    (OUT / (name + '.log')).write_text(result.stdout, encoding='utf-8')
    results.append({'gate': name, 'command': args, 'exit_code': result.returncode})
    print(f'=== {name}: {result.returncode} ===\n{result.stdout}', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)

try:
    if os.name != 'nt':
        raise SystemExit('This gate requires Windows/MSVC, not a cross compile.')
    run('provenance', ['git', 'log', '-1', '--format=commit=%H%ntree=%T%nparent=%P%nsubject=%s'])
    run('cmake-version', ['cmake', '--version'])
    run('win32-configure', ['cmake', '-S', '.', '-B', 'build-r6-production', '-A', 'Win32', '-DCLIENT_PATH='])
    run('warcraftxl-build', ['cmake', '--build', 'build-r6-production', '--config', 'Release', '--target', 'WarcraftXL', '--parallel', '4'])
    run('proxy-evidence-build', ['cmake', '--build', 'build-r6-production', '--config', 'Release', '--target', 'd3d9', '--parallel', '4'])
    for short, folder in [('r5', 'r5-receiver-check'), ('water', 'r6-water-check'), ('slot3', 'r6-slot3-check')]:
        build = 'build-r6-qualify-' + short
        run(short + '-configure', ['cmake', '-S', 'tools/' + folder, '-B', build, '-A', 'Win32'])
        run(short + '-build', ['cmake', '--build', build, '--config', 'Release', '--parallel', '4'])
        if short == 'r5':
            run('r5-receiver-smoke', [str(ROOT / build / 'Release' / 'R5ReceiverSmoke.exe')])
        else:
            run(short + '-tests', ['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure', '-V'])
    run('capture-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/r6-water-check', '-p', 'test_capture.py', '-v'])
    for name, folder in [('WarcraftXL.dll', 'assembly-not-deployable'), ('d3d9.dll', 'proxy-evidence-only-DO-NOT-DEPLOY')]:
        destination = OUT / folder / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'build-r6-production' / 'Release' / name, destination)
        digest = hashlib.sha256(destination.read_bytes()).hexdigest()
        destination.with_suffix('.dll.sha256').write_text(f'{digest}  {name}\n')
    for asm in (ROOT / 'build-r6-qualify-slot3').glob('*.asm'):
        shutil.copy2(asm, OUT / asm.name)
finally:
    (OUT / 'results.json').write_text(json.dumps({'platform': platform.platform(),
        'github_sha': os.getenv('GITHUB_SHA'), 'run_id': os.getenv('GITHUB_RUN_ID'),
        'gates': results, 'native_bridge': 'CONCRETE PARTIAL / FAIL-CLOSED: screen sampler descriptors and secondary native state closure remain unqualified',
        'live_test_ready': False}, indent=2) + '\n')
