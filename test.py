#!/usr/bin/env python3
import difflib
import os
import re
import shutil
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def change_dir(destination):
    try:
        old_dir = os.getcwd()
        os.chdir(destination)
        yield
    finally:
        os.chdir(old_dir)


def clean_lines(lines):
    return [x.strip() for x in lines if len(x.strip()) > 0]


def run_test(test_dir):
    print(f">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> {test_dir.name} <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<")
    pwd = subprocess.run('pwd', capture_output=True, text=True)
    stdout_expected = (Path.cwd() / 'stdout_expected').resolve()
    stderr_expected = (Path.cwd() / 'stderr_expected').resolve()

    fs = subprocess.run(
        [str(executable), 'input'],
        capture_output=True,
        text=True,
    )

    disks = [x for x in Path.cwd().glob('*') if re.match(r'^disk\d?$', x.name)]
    for disk in disks:
        diske = disk.parent / f"{disk.name}_expected"

        if not diske.exists():
            is_identical = True
        else:
            is_identical = disk.read_bytes() == diske.read_bytes()

        if is_identical:
            print(f"✅ {disk.name}: Matches expected disk")
        else:
            print(f"❌ {disk.name}: DOES NOT MATCH expected disk")

    with open(stdout_expected, 'r') as f:
        stdout_expected = f.readlines()
    with open(stderr_expected, 'r') as f:
        stderr_expected = f.readlines()

    differ = difflib.Differ()

    stdout_expected = clean_lines(stdout_expected)
    stdout_acc = clean_lines(fs.stdout.split('\n'))
    diff = list(differ.compare(stdout_expected, stdout_acc))
    nb_wrong = sum([int(x.startswith('+') or x.startswith('-')) for x in diff])
    if nb_wrong == 0:
        print("✅ STDOUT is correct")
    else:
        print("❌ ===== STDOUT DIFF, (+) extra line, (-) missing line, ( ) is correct =======")
        print('\n'.join(diff))
        print("==============================================================================")

    stderr_expected = clean_lines(stderr_expected)
    stderr_acc = clean_lines(fs.stderr.split('\n'))
    diff = list(differ.compare(stderr_expected, stderr_acc))
    nb_wrong = sum([int(x.startswith('+') or x.startswith('-')) for x in diff])
    if nb_wrong == 0:
        print("✅ STDERR is correct")
    else:
        print("❌ ===== STDERR DIFF, (+) extra line, (-) missing line, ( ) is correct =======")
        print('\n'.join(diff))
        print("==============================================================================")


if __name__ == '__main__':
    tests = list(Path('./tests').glob('test*'))
    executable = Path('./fs').resolve()

    for t in sorted(tests):
        with tempfile.TemporaryDirectory() as tmpdir:
            shutil.copytree(t, tmpdir, dirs_exist_ok=True)
            with change_dir(tmpdir):
                run_test(t)
