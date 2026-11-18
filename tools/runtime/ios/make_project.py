#!/usr/bin/env python3
"""Write the two Xcode projects of the iOS step (task 26), in the style of the TXRA port's tools/ios/make_project.py.

    python3 tools/ios/make_project.py           write both projects
    python3 tools/ios/make_project.py --check   fail if a written project is out of date
    python3 tools/ios/make_project.py --show-ldflags   print the app's OTHER_LDFLAGS as written, and as xcodebuild expands them with
                                                the environment's KHNMC_GFX_LDFLAGS (writes nothing)

  KHNMC_GFX_LDFLAGS (TASK38, IOS-GRAPHICS-PLAN G3): a build setting of the app target, EMPTY in the project, appended to its
  OTHER_LDFLAGS.  tools/ios/mac/build.sh and sign_app.sh with GFX=1 pass it on xcodebuild's command line (the four vkd3d archives,
  libMoltenVK.a, -lc++, Metal / QuartzCore / IOSurface / CoreGraphics: `make -s IOS_SDK=iphoneos GFX_VKD3D=1 print-GFX_APP_LDFLAGS`),
  because the app force-loads libkhnmc-rt.a, and a library built with the graphics backend links only with them.  Without it the link
  line is the one it always was.

  tools/ios/KHNMC.xcodeproj        the test shell: runtime/ios/ios_main.m linked with the game as two static libraries that
                                   `make IOS_SDK=<sdk> applib` leaves in obj-<sdk>/ (libkhnmc-rt.a force-loaded: every hand-written
                                   object, as the command-line link has them; libkh2loose.a: the lifted shards).  Bundle id
                                   beer.vyvanse.khnmc.
  tools/ios/KHNMCProbe.xcodeproj   the device probe (tools/ios/probe/): probe_main.m + probe_core.c, nothing of the game.  Bundle id
                                   beer.vyvanse.khnmc.probe.
Both: one app target, no schemes, arm64 only, iOS 17.0 or later, NO signing and NO entitlements (a signature would carry the
signer's identity; the Simulator runs unsigned apps, and signing for a device is the owner's own step), no build-machine paths
in the binary (-ffile-prefix-map).

UNRUN as far as Xcode goes: this generator runs anywhere (standard library only, object ids derived from names, so the output is
stable and can be diffed) and its output was written and checked on Linux, but no Xcode has opened these projects and nothing was
built from them.  The generator's structure is the one TXRA's projects are built from.
"""
import argparse
import hashlib
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def q(value):
    if isinstance(value, (list, tuple)):
        return '(\n' + ''.join(f'\t\t\t\t\t{q(v)},\n' for v in value) + '\t\t\t\t)'
    value = str(value)
    if re.fullmatch(r'[A-Za-z0-9_./]+', value):
        return value
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def settings(d):
    return '{\n' + ''.join(f'\t\t\t\t{k} = {q(v)};\n' for k, v in sorted(d.items())) + '\t\t\t}'


def file_type(name):
    return {'.c': 'sourcecode.c.c', '.h': 'sourcecode.c.h', '.m': 'sourcecode.c.objc', '.plist': 'text.plist.xml',
            '.xcassets': 'folder.assetcatalog'}[Path(name).suffix]


def generate(name, groups, target_settings):
    """name: product and target name; groups: [(title, [(path relative to tools/ios, display name)])]"""
    def oid(*parts):
        return hashlib.md5('/'.join((name.lower(),) + parts).encode()).hexdigest()[:24].upper()

    objects = {}  # section -> list of (id, comment, body)

    def add(section, ident, comment, body):
        objects.setdefault(section, []).append((ident, comment, body))

    app = oid('product')
    sources, resources, group_ids = [], [], []
    for title, files in groups:
        children = []
        for path, fname in files:
            ref = oid('file', path)
            tree = '"<group>"' if not path.startswith('../') else 'SOURCE_ROOT'
            add('PBXFileReference', ref, fname,
                f'{{isa = PBXFileReference; lastKnownFileType = {file_type(fname)}; name = {q(fname)}; '
                f'path = {q(path)}; sourceTree = {tree}; }}')
            children.append((ref, fname))
            if fname.endswith(('.c', '.m')):
                build = oid('build', path)
                add('PBXBuildFile', build, f'{fname} in Sources', f'{{isa = PBXBuildFile; fileRef = {ref} /* {fname} */; }}')
                sources.append((build, fname))
            elif fname.endswith('.xcassets'):          # the app icon (tools/ios/Assets.xcassets): compiled by actool into the bundle
                build = oid('build', path)
                add('PBXBuildFile', build, f'{fname} in Resources', f'{{isa = PBXBuildFile; fileRef = {ref} /* {fname} */; }}')
                resources.append((build, fname))
        gid = oid('group', title)
        kids = ''.join(f'\t\t\t\t{r} /* {n} */,\n' for r, n in children)
        add('PBXGroup', gid, title,
            f'{{\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n{kids}\t\t\t);\n\t\t\tname = {q(title)};\n'
            f'\t\t\tsourceTree = "<group>";\n\t\t}}')
        group_ids.append((gid, title))
    add('PBXFileReference', app, f'{name}.app',
        '{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; '
        f'path = {name}.app; sourceTree = BUILT_PRODUCTS_DIR; }}')
    products = oid('group', 'Products')
    add('PBXGroup', products, 'Products',
        f'{{\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n\t\t\t\t{app} /* {name}.app */,\n\t\t\t);\n'
        '\t\t\tname = Products;\n\t\t\tsourceTree = "<group>";\n\t\t}')
    main = oid('group', 'main')
    kids = ''.join(f'\t\t\t\t{g} /* {t} */,\n' for g, t in group_ids + [(products, 'Products')])
    add('PBXGroup', main, None,
        f'{{\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n{kids}\t\t\t);\n\t\t\tsourceTree = "<group>";\n\t\t}}')

    def phase(section, ident, pname, files):
        listing = ''.join(f'\t\t\t\t{b} /* {n} */,\n' for b, n in files)
        add(section, ident, pname,
            f'{{\n\t\t\tisa = {section};\n\t\t\tbuildActionMask = 2147483647;\n\t\t\tfiles = (\n{listing}'
            '\t\t\t);\n\t\t\trunOnlyForDeploymentPostprocessing = 0;\n\t\t}')
    src_phase, fw_phase, res_phase = oid('phase', 'sources'), oid('phase', 'frameworks'), oid('phase', 'resources')
    phase('PBXSourcesBuildPhase', src_phase, 'Sources', sources)
    phase('PBXFrameworksBuildPhase', fw_phase, 'Frameworks', [])
    phase('PBXResourcesBuildPhase', res_phase, 'Resources', resources)

    common = {
        'ALWAYS_SEARCH_USER_PATHS': 'NO',
        'ARCHS': 'arm64',
        'CLANG_ENABLE_MODULES': 'YES',
        'CLANG_ENABLE_OBJC_ARC': 'YES',
        # No signing at all: a signature would carry the signer's identity.  The Simulator runs unsigned apps.
        'CODE_SIGNING_ALLOWED': 'NO',
        'CODE_SIGNING_REQUIRED': 'NO',
        'CODE_SIGN_IDENTITY': '',
        'CODE_SIGN_STYLE': 'Manual',
        'DEVELOPMENT_TEAM': '',
        'DEBUG_INFORMATION_FORMAT': 'dwarf',
        'ENABLE_BITCODE': 'NO',
        'ENABLE_USER_SCRIPT_SANDBOXING': 'NO',
        'GCC_C_LANGUAGE_STANDARD': 'gnu11',
        'IPHONEOS_DEPLOYMENT_TARGET': '17.0',
        'ONLY_ACTIVE_ARCH': 'NO',
        'SDKROOT': 'iphoneos',
        'SUPPORTED_PLATFORMS': 'iphoneos iphonesimulator',
        'TARGETED_DEVICE_FAMILY': '1',
    }
    target = {
        'CURRENT_PROJECT_VERSION': '1',
        'GENERATE_INFOPLIST_FILE': 'NO',
        'MARKETING_VERSION': '0.1.0',
        'PRODUCT_NAME': name,
        'KHNMC_REPO': '$(SRCROOT)/../..',
        'WARNING_CFLAGS': ['-Wall', '-Wextra', '-Wno-unused-parameter'],
    }
    target.update(target_settings)
    debug = {'GCC_OPTIMIZATION_LEVEL': '0', 'GCC_GENERATE_DEBUGGING_SYMBOLS': 'YES'}
    # the symbol table stays: fault reports print host backtraces with it (runtime/core.c), and the link checks read it
    release = {'GCC_OPTIMIZATION_LEVEL': '1', 'GCC_GENERATE_DEBUGGING_SYMBOLS': 'NO', 'COPY_PHASE_STRIP': 'NO',
               'STRIP_INSTALLED_PRODUCT': 'NO', 'VALIDATE_PRODUCT': 'NO'}
    configs = {}
    for scope, base in (('project', common), ('target', target)):
        for cname, extra in (('Debug', debug), ('Release', release)):
            ident = oid('config', scope, cname)
            values = dict(base, **(extra if scope == 'target' else {}))
            add('XCBuildConfiguration', ident, cname,
                f'{{\n\t\t\tisa = XCBuildConfiguration;\n\t\t\tbuildSettings = {settings(values)};\n'
                f'\t\t\tname = {cname};\n\t\t}}')
            configs.setdefault(scope, []).append((ident, cname))
    lists = {}
    for scope, label in (('project', f'PBXProject "{name}"'), ('target', f'PBXNativeTarget "{name}"')):
        ident = oid('configlist', scope)
        items = ''.join(f'\t\t\t\t{i} /* {n} */,\n' for i, n in configs[scope])
        add('XCConfigurationList', ident, f'Build configuration list for {label}',
            f'{{\n\t\t\tisa = XCConfigurationList;\n\t\t\tbuildConfigurations = (\n{items}\t\t\t);\n'
            '\t\t\tdefaultConfigurationIsVisible = 0;\n\t\t\tdefaultConfigurationName = Release;\n\t\t}')
        lists[scope] = ident
    tgt = oid('target', name)
    add('PBXNativeTarget', tgt, name,
        f'{{\n\t\t\tisa = PBXNativeTarget;\n\t\t\tbuildConfigurationList = {lists["target"]};\n'
        f'\t\t\tbuildPhases = (\n\t\t\t\t{src_phase} /* Sources */,\n\t\t\t\t{fw_phase} /* Frameworks */,\n'
        f'\t\t\t\t{res_phase} /* Resources */,\n\t\t\t);\n\t\t\tbuildRules = (\n\t\t\t);\n'
        f'\t\t\tdependencies = (\n\t\t\t);\n\t\t\tname = {name};\n\t\t\tproductName = {name};\n'
        f'\t\t\tproductReference = {app} /* {name}.app */;\n'
        '\t\t\tproductType = "com.apple.product-type.application";\n\t\t}')
    proj = oid('project')
    add('PBXProject', proj, 'Project object',
        f'{{\n\t\t\tisa = PBXProject;\n\t\t\tattributes = {{\n\t\t\t\tBuildIndependentTargetsInParallel = 1;\n'
        '\t\t\t\tLastUpgradeCheck = 2700;\n\t\t\t};\n'
        f'\t\t\tbuildConfigurationList = {lists["project"]};\n\t\t\tcompatibilityVersion = "Xcode 14.0";\n'
        '\t\t\tdevelopmentRegion = en;\n\t\t\thasScannedForEncodings = 0;\n'
        '\t\t\tknownRegions = (\n\t\t\t\ten,\n\t\t\t\tBase,\n\t\t\t);\n'
        f'\t\t\tmainGroup = {main};\n\t\t\tproductRefGroup = {products} /* Products */;\n'
        '\t\t\tprojectDirPath = "";\n\t\t\tprojectRoot = "";\n'
        f'\t\t\ttargets = (\n\t\t\t\t{tgt} /* {name} */,\n\t\t\t);\n\t\t}}')

    out = ['// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tclasses = {\n\t};\n\tobjectVersion = 56;\n\tobjects = {\n']
    for section in sorted(objects):
        out.append(f'\n/* Begin {section} section */\n')
        for ident, comment, body in sorted(objects[section]):
            label = f' /* {comment} */' if comment else ''
            out.append(f'\t\t{ident}{label} = {body};\n')
        out.append(f'/* End {section} section */\n')
    out.append(f'\t}};\n\trootObject = {proj} /* Project object */;\n}}\n')
    return ''.join(out)


FRAMEWORKS = ['Foundation', 'UIKit', 'AVFoundation', 'AudioToolbox', 'GameController',   # audio: runtime/ios/audio_coreaudio.c (COREAUDIO=1 in mac/build.sh)
              'QuartzCore']   # TASK41: CAMetalLayer (the game view's layer) and CADisplayLink in runtime/ios/ios_main.m, in every app build
PREFIX_MAP = ['-ffp-contract=off', '-ffile-prefix-map=$(KHNMC_REPO)/=khnmc/']


def app_settings():
    return {'INFOPLIST_FILE': 'Info.plist',
            'PRODUCT_BUNDLE_IDENTIFIER': 'beer.vyvanse.khnmc',
            # the owner's icon (docs/APPLE-PLAN.md "App icon"): a single-size set; its PNG is NOT in the repository,
            # tools/ios/sync.sh copies it from ~/Documents/khnmc/app-icon/ into the Mac's copy of the catalog
            'ASSETCATALOG_COMPILER_APPICON_NAME': 'AppIcon',
            # obj-iphoneos / obj-iphonesimulator: what `make IOS_SDK=$(PLATFORM_NAME) applib` writes (Makefile)
            'KHNMC_LIBS': '$(KHNMC_REPO)/obj-$(PLATFORM_NAME)',
            'OTHER_CFLAGS': PREFIX_MAP,
            # TASK38: empty here; GFX=1 builds pass it to xcodebuild (see the top of this file)
            'KHNMC_GFX_LDFLAGS': '',
            'OTHER_LDFLAGS': ['-Wl,-force_load,$(KHNMC_LIBS)/libkhnmc-rt.a', '$(KHNMC_LIBS)/libkh2loose.a']
                             + [x for f in FRAMEWORKS for x in ('-framework', f)] + ['$(KHNMC_GFX_LDFLAGS)']}


def projects():
    app = generate('KHNMC',
                   [('App shell', [('../../runtime/ios/ios_main.m', 'ios_main.m')]),
                    ('App', [('Info.plist', 'Info.plist'), ('Assets.xcassets', 'Assets.xcassets')])],
                   app_settings())
    probe = generate('KHNMCProbe',
                     [('Probe', [('probe/probe_main.m', 'probe_main.m'), ('probe/probe_core.c', 'probe_core.c')]),
                      ('App', [('probe/Info.plist', 'Info.plist')])],
                     {'INFOPLIST_FILE': 'probe/Info.plist',
                      'PRODUCT_BUNDLE_IDENTIFIER': 'beer.vyvanse.khnmc.probe',
                      'OTHER_CFLAGS': PREFIX_MAP,
                      'OTHER_LDFLAGS': [x for f in FRAMEWORKS for x in ('-framework', f)]})
    out = [(HERE / 'KHNMC.xcodeproj' / 'project.pbxproj', app), (HERE / 'KHNMCProbe.xcodeproj' / 'project.pbxproj', probe)]
    # a shared scheme per project: xcodebuild accepts -derivedDataPath (which keeps Xcode's derived data inside our folder) only
    # together with -scheme, not with -target (found on the first real run, 2026-11-17)
    for name in ('KHNMC', 'KHNMCProbe'):
        out.append((HERE / f'{name}.xcodeproj' / 'xcshareddata' / 'xcschemes' / f'{name}.xcscheme', scheme(name)))
    return out


def scheme(name):
    tgt = hashlib.md5('/'.join((name.lower(), 'target', name)).encode()).hexdigest()[:24].upper()   # = generate()'s oid('target', name)
    ref = f'''<BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{tgt}" BuildableName = "{name}.app" BlueprintName = "{name}" ReferencedContainer = "container:{name}.xcodeproj"></BuildableReference>'''
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "2720" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "NO" buildForRunning = "YES" buildForProfiling = "NO" buildForArchiving = "YES" buildForAnalyzing = "NO">
            {ref}
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <LaunchAction buildConfiguration = "Release" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" allowLocationSimulation = "NO">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         {ref}
      </BuildableProductRunnable>
   </LaunchAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "NO"></ArchiveAction>
</Scheme>
'''


def sane(text):
    """what can be checked without Xcode: balanced braces / parentheses outside strings, every referenced object id defined"""
    depth = {'{': 0, '(': 0}; instr = False; prev = ''
    for ch in text:
        if instr:
            if ch == '"' and prev != '\\': instr = False
        elif ch == '"': instr = True
        elif ch in '{(': depth[ch] += 1
        elif ch == '}': depth['{'] -= 1
        elif ch == ')': depth['('] -= 1
        prev = ch
    if instr or depth['{'] or depth['(']:
        return 'unbalanced braces, parentheses or quotes'
    defined = set(re.findall(r'^\t\t([0-9A-F]{24})(?: /\*.*?\*/)? = \{', text, re.M))
    used = set(re.findall(r'\b([0-9A-F]{24})\b', text))
    if used - defined:
        return 'object ids used but not defined: %s' % ' '.join(sorted(used - defined))
    for path in re.findall(r'path = ("[^"]*"|[^;]+); sourceTree = (?:SOURCE_ROOT|"<group>")', text):
        p = (HERE / path.strip('"')).resolve()
        if not p.exists():
            return 'file reference %s does not exist' % path
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--check', action='store_true')
    p.add_argument('--show-ldflags', action='store_true')
    a = p.parse_args()
    if a.show_ldflags:
        import os
        import shlex
        written = app_settings()['OTHER_LDFLAGS']
        gfx = os.environ.get('KHNMC_GFX_LDFLAGS', '')
        print('make_project: KHNMC OTHER_LDFLAGS as written: ' + ' '.join(written))
        print('make_project: KHNMC_GFX_LDFLAGS = %s' % (gfx if gfx.strip() else '(empty: the link line it always was)'))
        out = []
        for x in written:
            out += shlex.split(gfx) if x == '$(KHNMC_GFX_LDFLAGS)' else [x]
        print('make_project: KHNMC OTHER_LDFLAGS expanded (%d words): %s' % (len(out), ' '.join(out)))
        return
    for path, text in projects():
        bad = sane(text) if path.suffix == '.pbxproj' else (None if '<Scheme' in text and 'BlueprintIdentifier' in text else 'bad scheme text')
        if bad:
            sys.exit('%s: generator error: %s' % (path.relative_to(ROOT), bad))
        if a.check:
            if not path.is_file() or path.read_text() != text:
                sys.exit('%s is out of date: run python3 tools/ios/make_project.py' % path.relative_to(ROOT))
            print('%s is up to date' % path.relative_to(ROOT))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        print(path.relative_to(ROOT))


if __name__ == '__main__':
    main()
