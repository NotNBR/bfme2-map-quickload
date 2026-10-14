"""Build bin/dinput8.dll and bin/loading.dll from loading.cpp.

    python build.py [--toolchain FOLDER]

FOLDER is a Visual C++ .NET 2003 (MSVC 7.1) installation, the folder holding
Vc7/ and Common7/ (default: the MSVC71 environment variable). MSVC 7.1 links
msvcr71.dll, which BFME2 ships; newer compilers are untested.
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
# output: (compiler defines, exports as name=symbol)
BUILDS = {
    'dinput8.dll': (['MW_STANDALONE'], ['DirectInput8Create=_DirectInput8Create@20']),   # the normal game
    'loading.dll': ([], []),                                                              # launchers
}


def build(toolchain, out=HERE/'bin'):
    root = Path(toolchain)
    vc = root/'Vc7'
    if not (vc/'bin/cl.exe').is_file():
        raise SystemExit(f'No Visual C++ .NET 2003 in {root} (expected Vc7/bin/cl.exe)')
    # Some toolchain copies keep msvcr71.dll and msvcp71.dll two folders up.
    env = dict(os.environ, PATH=os.pathsep.join(map(str, [vc/'bin', root/'Common7/IDE', root.parents[1]]))
               + os.pathsep + os.environ.get('PATH', ''),
               INCLUDE=os.pathsep.join(map(str, [vc/'include', vc/'PlatformSDK/Include'])),
               LIB=os.pathsep.join(map(str, [vc/'lib', vc/'PlatformSDK/Lib'])))
    out.mkdir(parents=True, exist_ok=True)
    built = []
    for name, (defines, exports) in BUILDS.items():
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            subprocess.run([str(vc/'bin/cl.exe'), '/nologo', '/O2', '/MD', '/LD', *[f'/D{d}' for d in defines],
                            str(HERE/'loading.cpp'), '/Fe' + str(work/name), '/Fo' + str(work/'loading.obj'),
                            '/link', '/MACHINE:X86', 'kernel32.lib', 'user32.lib', *[f'/EXPORT:{e}' for e in exports]],
                           env=env, cwd=work, check=True)
            shutil.copy2(work/name, out/name)
        built.append(out/name)
    return built


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--toolchain', default=os.environ.get('MSVC71'), help='Visual C++ .NET 2003 folder')
    args = parser.parse_args()
    if not args.toolchain:
        parser.error('give --toolchain or set MSVC71 to the Visual C++ .NET 2003 folder')
    for path in build(args.toolchain):
        print(path)
