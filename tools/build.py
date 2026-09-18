#!/usr/bin/env python3
"""Build our probe without loading, signing, changing security or resetting hardware."""
from pathlib import Path
import argparse
import plistlib
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description='Build or offline-test the native CX5 driver; neither action installs it.')
parser.add_argument('mode',choices=['test','native'])
args=parser.parse_args()
BUILD=ROOT/'build'
BUILD.mkdir(exist_ok=True)
def run(args):
    subprocess.run([str(x) for x in args],check=True,cwd=ROOT)
def sdk(name):
    return subprocess.check_output(['xcrun','--sdk',name,'--show-sdk-path'],text=True).strip()
def plist(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(plistlib.dumps(data,sort_keys=False))
if len(sys.argv)>1 and sys.argv[1]=='test':
    for compiler,standard,language in [('clang','c11','c'),('clang++','c++17','c++')]:
        target=BUILD/('test-apple-build-ids-'+language)
        run([compiler,'-x',language,'-std='+standard,'-O2','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-Iinclude','tests/test_apple_build_ids.c','-o',target])
        run([target])
    run(['clang++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-Iinclude','tests/test_command_wait.cpp','-o',BUILD/'test-command-wait'])
    run([BUILD/'test-command-wait'])
    run(['clang++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-Iinclude','tests/test_pointer_index.cpp','-o',BUILD/'test-pointer-index'])
    run([BUILD/'test-pointer-index'])
    for name in ['protocol','mailboxes','verbs','user_mkey','work_queue']:
        run(['clang++','-std=c++17','-O2','-fno-strict-aliasing','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
            '-Iinclude','core/cx5_protocol.cpp','core/cx5_verbs.cpp','tests/test_'+name+'.cpp','-o',BUILD/('test-'+name)])
        run([BUILD/('test-'+name)])
    # Uses the real HCA implementation and codec with simulated firmware/MMIO.
    # Unused private transport fields are expected only in this test double.
    run(['clang++','-std=c++17','-O2','-fno-strict-aliasing','-Wall','-Wextra','-Werror',
         '-Wno-unused-private-field','-fsanitize=address,undefined','-g',
         '-DCX5_NATIVE_TEST','-Iinclude','-Inative','-Itests',
         'core/cx5_protocol.cpp','core/cx5_verbs.cpp','native/kernel_hca.cpp','native/apple_data_verbs.cpp',
         'native/kernel_transport_access.cpp','tests/fake_kernel_transport.cpp','tests/test_kernel_hca.cpp','-o',BUILD/'test-kernel-hca'])
    run([BUILD/'test-kernel-hca'])
    run(['clang++','-std=c++17','-O2','-fno-strict-aliasing','-Wall','-Wextra','-Werror',
         '-Wno-unused-private-field','-fsanitize=address,undefined','-g',
         '-DCX5_NATIVE_TEST','-Iinclude','-Inative','-Itests',
         'core/cx5_protocol.cpp','core/cx5_verbs.cpp','native/kernel_hca.cpp',
         'native/apple_data_verbs.cpp','native/apple_provider.cpp','native/cq_mapping.cpp','native/registered_memory.cpp',
         'native/kernel_transport_access.cpp','tests/fake_kernel_transport.cpp','tests/test_apple_provider.cpp','-o',BUILD/'test-apple-provider'])
    run([BUILD/'test-apple-provider'])
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-Iinclude','tests/test_user_completion.c','-o',BUILD/'test-user-completion'])
    run([BUILD/'test-user-completion'])
    run(['clang++','-std=c++17','-O2','-fno-strict-aliasing','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
         '-Iinclude','core/cx5_protocol.cpp','core/cx5_verbs.cpp','tests/test_user_post.cpp','-o',BUILD/'test-user-post'])
    run([BUILD/'test-user-post'])
    # Includes the real userspace provider, substitutes only command polling,
    # and uses ordinary memory for mappings; no device is opened or accessed.
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         'tests/test_user_provider.c','-lrdma','-o',BUILD/'test-user-provider'])
    run([BUILD/'test-user-provider'])
    run([sys.executable,'tests/test_native_observer_marker.py'])
    run([sys.executable,'tests/test_native_endpoint.py'])
    run([sys.executable,'-B','tests/test_restore_rdma.py'])
    run([sys.executable,'-B','tests/test_lifecycle_torture.py'])
    run([sys.executable,'-B','tests/test_bw_tools.py'])
    run([sys.executable,'-B','tests/test_bw_guard.py'])
    sys.exit(0)
if len(sys.argv)>1 and sys.argv[1]=='native':
    ms=sdk('macosx')
    version=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-version'],text=True).strip()
    if int(version.split('.')[0])!=27:
        sys.exit('Native ABI2 builds are pinned to the macOS 27 SDK; exact inherited imports also require apple_vm_abi.hpp')
    run(['clang','-std=c11','-Wall','-Wextra','-Werror','-isysroot',ms,
         'client/cx5_native_check.c','-lrdma','-o',BUILD/'cx5-native-check'])
    run(['clang','-std=c11','-Wall','-Wextra','-Werror','-isysroot',ms,
         'peer/verbs_peer.c','-lrdma','-o',BUILD/'native-verbs-peer'])
    run(['clang','-std=c11','-arch','arm64e','-Wall','-Wextra','-Werror','-isysroot',ms,
         '-O2','-dynamiclib','native/user_provider.c','-lrdma','-o',BUILD/'libmcdma-rdmav34.so'])
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-isysroot',ms,
         'client/cq_map_check.c','-lrdma','-o',BUILD/'cq-map-check'])
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-isysroot',ms,
         'client/user_queue_check.c','-lrdma','-o',BUILD/'user-queue-check'])
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-isysroot',ms,
         'client/lifecycle_client.c','-lrdma','-o',BUILD/'lifecycle-client'])
    # Sustained-bandwidth benchmark; the same source builds on the Linux peer with -libverbs.
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-isysroot',ms,
         'benchmarks/mcdma_bw.c','-lrdma','-o',BUILD/'mcdma-bw'])
    run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-isysroot',ms,
         'client/mcdma_set.c','-framework','IOKit','-framework','CoreFoundation','-o',BUILD/'mcdma-set'])
    # GPU keep-alive: holds the platform out of its idle power state during
    # latency-critical RDMA (see docs/gpu-keepalive.md).
    run(['xcrun','swiftc','-O','-sdk',ms,'client/fabric_keepalive.swift','-o',BUILD/'fabric-keepalive'])
    native_sources=['native/apple_registration.cpp','native/kernel_transport.cpp',
                    'native/kernel_transport_access.cpp',
                    'native/kernel_hca.cpp','native/apple_umem.cpp','native/registered_memory.cpp',
                    'native/apple_data_verbs.cpp','native/apple_provider.cpp','native/cq_mapping.cpp',
                    'native/rdma_network.cpp','native/MCDMACX5Native.cpp','native/module.cpp',
                    'core/cx5_protocol.cpp','core/cx5_verbs.cpp']
    native_objects=[]
    for source in native_sources:
        target=BUILD/('native-'+Path(source).stem+'.o')
        native_objects.append(target)
        run(['xcrun','clang++','-arch','arm64e','-std=c++17','-mkernel','-fapple-kext',
             '-O2','-fno-strict-aliasing','-mgeneral-regs-only',
             '-fno-exceptions','-fno-rtti','-DKERNEL','-DKERNEL_PRIVATE',
             '-include','native/apple_vm_abi.hpp',
             '-Wall','-Wextra','-Werror','-Wno-unused-parameter',
             '-Wno-deprecated-declarations','-Iinclude','-isystem',
             ms+'/System/Library/Frameworks/Kernel.framework/Headers',
             '-c',source,'-o',target])
    run(['xcrun','libtool','-static','-o',BUILD/'libcx5-kernel-backend.a',*native_objects])
    kext=BUILD/'MCDMACX5Native.kext'
    (kext/'Contents/MacOS').mkdir(parents=True,exist_ok=True)
    run(['xcrun','clang++','-arch','arm64e','-nostdlib','-isysroot',ms,
         '-mmacosx-version-min=27.0','-Wl,-kext','-Wl,-undefined,dynamic_lookup',*native_objects,
         '-lkmod','-lkmodc++','-o',kext/'Contents/MacOS/MCDMACX5Native'])
    imports=subprocess.check_output(['xcrun','nm','-uj',str(kext/'Contents/MacOS/MCDMACX5Native')],text=True)
    for symbol in ['__ZN18IOMemoryDescriptor5doMapEP7_vm_mapPyjyy',
                   '__ZN18IOMemoryDescriptor7doUnmapEP7_vm_mapyy']:
        if symbol not in imports.splitlines():
            sys.exit('Missing exact-build VM-map ABI import: '+symbol)
    if 'doMapEP8ipc_port' in imports or 'doUnmapEP8ipc_port' in imports:
        sys.exit('Incompatible public-SDK VM-map aliases remain in native vtable imports')
    plist(kext/'Contents/Info.plist',{
        'CFBundleInfoDictionaryVersion':'6.0','CFBundleVersion':'0.1.18',
        'CFBundleShortVersionString':'0.1.18','CFBundleIdentifier':'org.mcdma.cx5.native',
        'CFBundleExecutable':'MCDMACX5Native','CFBundleName':'MCDMA native CX5',
        'CFBundlePackageType':'KEXT',
        'OSBundleLibraries':{'com.apple.kpi.bsd':'27.0.0',
                            'com.apple.kpi.iokit':'27.0.0',
                            'com.apple.kpi.libkern':'27.0.0',
                            'com.apple.kpi.mach':'27.0.0',
                            'com.apple.kpi.unsupported':'27.0.0',
                            'com.apple.iokit.IOPCIFamily':'2.9',
                            'com.apple.iokit.IORDMAFamily':'1.0'},
        'IOKitPersonalities':{'MCDMACX5Native':{
            'CFBundleIdentifier':'org.mcdma.cx5.native','IOClass':'MCDMACX5Native',
            'IOProviderClass':'IOPCIDevice','IOPCIMatch':'0x101915b3 0x101515b3',
            'IOPCITunnelCompatible':True,'IOProbeScore':20000,
            'MCDMALabEnabled':False,'MCDMAUserQueues':False,'MCDMAUserBlueFlame':False,
            'MCDMARelaxedOrdering':False,'MCDMAAckRequestEveryPacket':False,'MCDMAMaxReadRequestBytes':0}}})
    print('BUILT unsigned native kernel bundle (-O2, general registers only), provider and checker; '
          'lab personality is disabled, live loading and verbs transfers remain unverified')
    sys.exit(0)
