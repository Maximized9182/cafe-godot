import os
import subprocess
import sys
from typing import TYPE_CHECKING

from SCons.Util import WhereIs

from methods import print_error
from platform_methods import validate_arch

if TYPE_CHECKING:
    from SCons.Script.SConscript import SConsEnvironment


def get_name():
    return "Wii U"


def _devkitpro():
    return os.environ.get("DEVKITPRO", "/opt/devkitpro")


def can_build():
    dkp = _devkitpro()
    if not os.path.isfile(os.path.join(dkp, "devkitPPC", "bin", "powerpc-eabi-g++")):
        return False
    if not os.path.isdir(os.path.join(dkp, "wut", "include")):
        return False
    return True


def get_tools(env: "SConsEnvironment"):
    # Don't let SCons probe for host compilers; we set the cross toolchain in configure().
    return ["cc", "c++", "ar", "link", "textfile", "zip"]


def get_opts():
    from SCons.Variables import BoolVariable

    return [
        BoolVariable("wiiu_udp_log", "Also send engine output to wiiu-udplogserver over the network", True),
    ]


def get_doc_classes():
    return []


def get_doc_path():
    return "doc_classes"


def get_flags():
    return {
        "arch": "ppc32",
        "target": "template_debug",
        # No GPU backend yet: the engine runs headless with the dummy renderer.
        "vulkan": False,
        "opengl3": False,
        "d3d12": False,
        "metal": False,
        "builtin_pcre2_with_jit": False,
        # Raycast needs Embree (x86/arm only).
        "module_raycast_enabled": False,
        "optimize": "size",
    }


def configure(env: "SConsEnvironment"):
    validate_arch(env["arch"], get_name(), ["ppc32"])

    if env.editor_build:
        print_error("The Wii U platform can only build export templates (target=template_debug/template_release).")
        sys.exit(255)

    dkp = _devkitpro()
    ppc_bin = os.path.join(dkp, "devkitPPC", "bin")
    wut = os.path.join(dkp, "wut")

    env["ENV"]["PATH"] = ppc_bin + os.pathsep + os.path.join(dkp, "tools", "bin") + os.pathsep + env["ENV"]["PATH"]
    env["ENV"]["DEVKITPRO"] = dkp

    # Code is compiled with Clang, not devkitPPC's GCC: wut has no native thread-local storage,
    # and only Clang can lower `thread_local` to emulated TLS (__emutls_get_address, which
    # devkitPPC's libgcc provides on top of wut's threads). Linking still goes through
    # devkitPPC's GCC driver so wut.specs, newlib, libstdc++ and libgcc are used unchanged.
    #
    # Clang's own powerpc-eabi target uses a different size_t (unsigned long) than GCC's
    # (unsigned int), which breaks C++ name mangling against libstdc++. The 32-bit
    # powerpc-linux target matches GCC's type sizes, so it's used with the Linux macros removed.
    clang = WhereIs("clang")
    if clang is None:
        print_error("Clang is required to build for Wii U (it provides emulated thread-local storage).")
        sys.exit(255)
    resource_dir = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()

    ppc_root = os.path.join(dkp, "devkitPPC")
    cxx_root = os.path.join(ppc_root, "powerpc-eabi", "include", "c++")
    gcc_versions = sorted(os.listdir(cxx_root))
    if not gcc_versions:
        print_error("No libstdc++ headers found in %s." % cxx_root)
        sys.exit(255)
    cxx_inc = os.path.join(cxx_root, gcc_versions[-1])

    env["CC"] = "clang"
    env["CXX"] = "clang++"
    env["LINK"] = "powerpc-eabi-g++"
    env["AR"] = "powerpc-eabi-ar"
    env["RANLIB"] = "powerpc-eabi-ranlib"
    env["AS"] = "powerpc-eabi-as"

    env["OBJSUFFIX"] = ".o"
    env["LIBPREFIX"] = "lib"
    env["LIBSUFFIX"] = ".a"
    env["PROGSUFFIX"] = ".elf"

    clang_target = [
        "--target=powerpc-unknown-linux-gnu",
        "-mcpu=750",
        "-mhard-float",
        "-mlong-double-64",  # devkitPPC's long double is 64-bit.
        "-femulated-tls",
        "-fno-pic",
        # wut's linker script merges .init_array into the .ctors list, which GCC's
        # EABI startup (__do_global_ctors_aux) runs backwards. Emit .ctors so the
        # order comes out right; with .init_array, static constructors ran in
        # reverse (StringName::allocator was configured after first use, leaving
        # page_size 0 and corrupting the heap).
        "-fno-use-init-array",
        # Clang's ppc32-linux stack protector reads its canary from r2 (the TLS
        # pointer on Linux), but on Cafe OS r2 is the SDA2 base.
        "-fno-stack-protector",
    ]
    not_linux = ["__linux__", "__linux", "linux", "__gnu_linux__", "__unix__", "__unix", "unix"]
    env.Append(CCFLAGS=clang_target + ["-U" + m for m in not_linux])
    env.Append(CCFLAGS=["-ffunction-sections", "-fdata-sections"])
    # Use devkitPPC's headers (libstdc++, newlib) and wut's, with Clang's builtin headers last.
    env.Append(CCFLAGS=["-nostdinc++", "-nostdlibinc"])
    env.Append(CXXFLAGS=["-isystem", cxx_inc, "-isystem", os.path.join(cxx_inc, "powerpc-eabi")])
    env.Append(
        CCFLAGS=[
            "-isystem",
            os.path.join(ppc_root, "powerpc-eabi", "include"),
            "-isystem",
            os.path.join(wut, "include"),
            "-isystem",
            os.path.join(resource_dir, "include"),
        ]
    )
    env.Append(CPPDEFINES=["ESPRESSO", "__WIIU__", "__WUT__"])

    env.Append(LINKFLAGS=["-mcpu=750", "-meabi", "-mhard-float", "-specs=" + os.path.join(wut, "share", "wut.specs")])
    env.Append(LIBPATH=[os.path.join(wut, "lib")])

    # LTO would need Clang's linker plugin in devkitPPC's ld.
    env["lto"] = "none"

    # compat/ holds stub POSIX headers newlib lacks (dlfcn.h, syslog.h, sys/xattr.h, sys/resource.h).
    env.Prepend(CPPPATH=["#platform/wiiu", "#platform/wiiu/compat"])
    env.Append(
        CPPDEFINES=[
            "WIIU_ENABLED",
            "UNIX_ENABLED",
            # No BSD sockets wired up yet (wut's nsysnet can provide them later).
            "UNIX_SOCKET_UNAVAILABLE",
            # newlib's pthread layer in wut has no pthread_setname_np.
            "PTHREAD_NO_RENAME",
        ]
    )
    if env["wiiu_udp_log"]:
        env.Append(CPPDEFINES=["WIIU_UDP_LOG"])

    # Linked last so the platform's own objects can resolve against them.
    env.Append(LIBS=["wut", "m"])
