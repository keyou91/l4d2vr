"""Build an x86 review candidate without writing to the checkout or game."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

MINHOOK_URL = 'https://github.com/TsudaKageyu/minhook.git'
MINHOOK_COMMIT = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
MSBUILD_NAMESPACE = 'http://schemas.microsoft.com/developer/msbuild/2003'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(command, cwd=None, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.stdout.decode('utf-8-sig', errors='replace').strip()


def visual_studio_environment():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
        'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise RuntimeError('Install Visual Studio 2022 C++ Build Tools and a Windows SDK.')
    installation = capture([str(vswhere), '-utf8', '-latest', '-products', '*', '-version', '[17,18)',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property', 'installationPath'])
    if not installation:
        raise RuntimeError('Visual Studio 2022 C++ tools were not found.')
    installation = Path(installation)
    developer = installation / 'Common7/Tools/VsDevCmd.bat'
    # The returned environment is captured privately, never written to build logs.
    if any(character in str(developer) for character in ('"', '\r', '\n')):
        raise RuntimeError('The Visual Studio path contains unsupported command characters.')
    result = subprocess.run(
        f'cmd.exe /d /u /s /c ""{developer}" -no_logo -arch=x86 -host_arch=x64 >nul && set"',
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    environment = {key.upper(): value for key, value in os.environ.items()}
    for line in result.stdout.decode('utf-16-le').splitlines():
        key, separator, value = line.partition('=')
        if separator and key and not key.startswith('='):
            environment[key.upper()] = value
    return installation / 'MSBuild/Current/Bin/MSBuild.exe', environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-tests', action='store_true',
        help='Build only the native candidate; run the separate interaction CI job.')
    args = parser.parse_args()
    if sys.platform != 'win32':
        raise RuntimeError('This candidate requires Windows and the x86 Visual Studio toolchain.')
    root = Path(__file__).resolve().parents[1]
    if not (root / 'l4d2vr.sln').is_file():
        raise RuntimeError('Run this script from the L4D2VR repository.')
    for command in ('git', 'cmake'):
        if not shutil.which(command):
            raise RuntimeError(f'{command} is required in PATH.')
    temporary_keys = ('RUNNER_TEMP', 'TMPDIR', 'TEMP', 'TMP')
    # Native tools can prefer TMP/TEMP even when Python prefers TMPDIR.
    # Reject every configured provider that could make a tool write to the repo.
    for key in temporary_keys:
        if os.environ.get(key) and Path(os.environ[key]).resolve().is_relative_to(root):
            raise RuntimeError(f'{key} must point outside the checkout.')
    temporary_setting = next((os.environ[key] for key in temporary_keys if os.environ.get(key)), None)
    if not temporary_setting:
        raise RuntimeError('Configure a candidate temporary directory outside the checkout.')
    temporary_base = Path(temporary_setting).resolve()
    if temporary_base == root or temporary_base.is_relative_to(root):
        raise RuntimeError('The candidate temporary directory must be outside the checkout.')
    if not temporary_base.is_dir():
        raise RuntimeError('The candidate temporary directory does not exist.')
    msbuild, environment = visual_studio_environment()
    scratch = Path(tempfile.mkdtemp(prefix='l4d2vr-candidate-',
        dir=temporary_base)).resolve()
    if not scratch.is_relative_to(temporary_base) or scratch.is_relative_to(root):
        raise RuntimeError('Candidate staging escaped its temporary directory.')
    source = scratch / 'source'
    source.mkdir()
    logs = scratch / 'logs'
    logs.mkdir()
    head = capture(['git', '-C', str(root), 'rev-parse', 'HEAD'])
    dirty = capture(['git', '-C', str(root), 'status', '--porcelain'])
    paths = capture(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others',
        '--exclude-standard', '--', 'l4d2vr.sln', 'L4D2VR', 'dxvk_new', 'thirdparty',
        'tests', 'tools', 'docs']).split('\0')
    hashes = {}
    for name in sorted(set(paths)):
        if not name:
            continue
        original = (root / name).resolve()
        target = (source / name).resolve()
        if not original.is_relative_to(root) or not target.is_relative_to(source):
            raise RuntimeError(f'Source path escapes the checkout: {name}')
        if not original.is_file():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        content = original.read_bytes()
        hashes[name] = hashlib.sha256(content).hexdigest()
        target.write_bytes(content)

    def run(label, command):
        print(label, flush=True)
        # CreateProcess resolves the executable before applying the child's PATH.
        # Resolve the x86 compiler/link tools in the captured developer environment.
        executable = shutil.which(command[0], path=environment['PATH'])
        if not executable:
            raise RuntimeError(f'{command[0]} was not found in the developer environment.')
        command = [executable, *command[1:]]
        log = logs / f'{label}.log'
        with log.open('wb') as output:
            result = subprocess.run(command, cwd=source, env=environment,
                stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'{label} failed. Read {log}')

    try:
        # Build the dependency with this compiler instead of reusing an older
        # LTCG archive. Fetch only the reviewed immutable upstream commit.
        dependency = scratch / 'minhook-source'
        run('minhook-init', ['git', 'init', str(dependency)])
        run('minhook-remote', ['git', '-C', str(dependency), 'remote', 'add', 'origin', MINHOOK_URL])
        run('minhook-fetch', ['git', '-C', str(dependency), 'fetch', '--depth', '1', 'origin', MINHOOK_COMMIT])
        run('minhook-checkout', ['git', '-C', str(dependency), 'checkout', '--detach', 'FETCH_HEAD'])
        if capture(['git', '-C', str(dependency), 'rev-parse', 'HEAD']) != MINHOOK_COMMIT:
            raise RuntimeError('MinHook checkout does not match the pinned commit.')
        dependency_build = scratch / 'minhook-build'
        run('minhook-configure', ['cmake', '-S', str(dependency), '-B', str(dependency_build),
            '-G', 'Visual Studio 17 2022', '-A', 'Win32', '-DBUILD_SHARED_LIBS=OFF',
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'])
        run('minhook-build', ['cmake', '--build', str(dependency_build), '--config', 'Release',
            '--', '/m:1', '/nr:false'])
        minhook_library = dependency_build / 'Release/minhook.x32.lib'
        if not minhook_library.is_file():
            raise RuntimeError('The pinned MinHook x86 library was not produced.')
        shutil.copyfile(minhook_library, source / 'thirdparty/minhook/lib/Release/libMinHook.x86.lib')

        # The repository ships a D3DX proxy source, so no legacy SDK is needed
        # for compilation. D3DX9_43.dll remains a game runtime prerequisite.
        proxy = source / 'thirdparty/directx/d3dx9'
        proxy_build = scratch / 'd3dx9-build'
        proxy_build.mkdir()
        run('d3dx9-compile', ['cl.exe', '/nologo', '/c', '/EHsc', '/MD', '/std:c++17',
            f'/I{proxy / "include"}', str(proxy / 'src/d3dx9_proxy.cpp'),
            f'/Fo{proxy_build / "d3dx9_proxy.obj"}'])
        proxy_library = proxy / 'lib/x86/d3dx9.lib'
        proxy_library.parent.mkdir(parents=True, exist_ok=True)
        run('d3dx9-library', ['lib.exe', '/nologo', f'/OUT:{proxy_library}',
            str(proxy_build / 'd3dx9_proxy.obj')])
        ET.register_namespace('', MSBUILD_NAMESPACE)
        project = source / 'L4D2VR/l4d2vr.vcxproj'
        tree = ET.parse(project)
        for parent in tree.iter():
            for child in list(parent):
                if child.tag == f'{{{MSBUILD_NAMESPACE}}}PostBuildEvent':
                    parent.remove(child)
        tree.write(project, encoding='utf-8', xml_declaration=True)
        run('native-release-x86', [str(msbuild), str(source / 'l4d2vr.sln'), '/t:Rebuild',
            '/p:Configuration=Release', '/p:Platform=x86', '/p:PlatformToolset=v143',
            '/p:PostBuildEventUseInBuild=false', '/p:CL_MPCount=2', '/m:1', '/nr:false',
            '/nologo', '/v:minimal'])

        suites = None
        if not args.skip_tests:
            test_build = scratch / 'tests-build'
            run('tests-configure', ['cmake', '-S', str(source / 'tests'), '-B', str(test_build),
                '-G', 'Visual Studio 17 2022', '-A', 'Win32',
                f'-DPython3_EXECUTABLE={sys.executable}'])
            suites = len(json.loads(capture(['ctest', '--test-dir', str(test_build),
                '-C', 'Release', '--show-only=json-v1']))['tests'])
            for configuration in ('Release', 'Debug'):
                run(f'tests-build-{configuration.lower()}', ['cmake', '--build', str(test_build),
                    '--config', configuration, '--', '/m:1', '/nr:false'])
                run(f'tests-run-{configuration.lower()}', ['ctest', '--test-dir', str(test_build),
                    '-C', configuration, '--output-on-failure', '--no-tests=error'])
        for name, expected in hashes.items():
            if sha(root / name) != expected:
                raise RuntimeError(f'Source changed during the candidate build: {name}')
        if capture(['git', '-C', str(root), 'rev-parse', 'HEAD']) != head or \
                capture(['git', '-C', str(root), 'status', '--porcelain']) != dirty:
            raise RuntimeError('Checkout identity changed during the candidate build.')

        candidate = scratch / 'candidate'
        candidate.mkdir()
        artifacts = {}
        for name in ('d3d9.dll', 'd3d9.pdb', 'l4d2vr_pose_server.dll', 'l4d2vr_pose_server.pdb'):
            target = candidate / name
            shutil.copyfile(source / 'Release' / name, target)
            if name.endswith('.dll'):
                content = target.read_bytes()
                pe = int.from_bytes(content[60:64], 'little') if len(content) >= 64 else 0
                if content[:2] != b'MZ' or content[pe:pe + 4] != b'PE\0\0' or \
                        int.from_bytes(content[pe + 4:pe + 6], 'little') != 0x14c:
                    raise RuntimeError(f'{name} is not a valid Windows x86 binary.')
            artifacts[name] = sha(target)
        shutil.copyfile(source / 'L4D2VR/l4d2vr_pose_server.vdf', candidate / 'l4d2vr_pose_server.vdf')
        shutil.copyfile(source / 'docs/manual-reload-input.md', candidate / 'manual-reload-input.md')
        shutil.copyfile(source / 'docs/weapon-coverage.md', candidate / 'weapon-coverage.md')
        shutil.copyfile(source / 'docs/playtest-candidate.md', candidate / 'playtest-candidate.md')
        reference = candidate / 'runtime-reference'
        reference.mkdir()
        shutil.copytree(source / 'L4D2VR/SteamVRActionManifest', reference / 'SteamVRActionManifest')
        shutil.copyfile(source / 'L4D2VR/config.txt', reference / 'config.default.txt')
        licenses = candidate / 'licenses'
        licenses.mkdir()
        shutil.copyfile(dependency / 'LICENSE.txt', licenses / 'MinHook.txt')
        shutil.copyfile(source / 'dxvk_new/LICENSE', licenses / 'DXVK.txt')
        shutil.copyfile(source / 'thirdparty/ozz-animation-install/win32/LICENSE.md', licenses / 'ozz-animation.md')
        result = {'head': head, 'working_tree_changes': bool(dirty), 'source_hashes': hashes,
            'minhook_commit': MINHOOK_COMMIT, 'minhook_library_sha256': sha(minhook_library),
            'd3dx9_proxy_sha256': sha(proxy_library), 'artifacts': artifacts,
            'native_release_x86_rebuild': True, 'release_suites': suites, 'debug_suites': suites,
            'runtime_verified': False, 'installed': False, 'published': False,
            'scratch': str(scratch), 'candidate': str(candidate)}
        (candidate / 'verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps({key: result[key] for key in ('head', 'candidate', 'native_release_x86_rebuild',
            'release_suites', 'debug_suites', 'working_tree_changes')}, indent=2), flush=True)
        # CI receives only the path, without dumping local source inventories.
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
                output.write(f'candidate={candidate}\nlogs={logs}\n')
    except Exception:
        print(f'Candidate evidence retained at {scratch}', file=sys.stderr, flush=True)
        raise


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
