import os
import shutil
import sys
import subprocess
import urllib.request
from setuptools import setup, Extension, find_packages

HERE = os.path.abspath(os.path.dirname(__file__))
ROOT_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))


def _check_compiler():
    cxx = os.environ.get("CXX", "g++")
    try:
        result = subprocess.run(
            [cxx, "--version"],
            capture_output=True,
            text=True,
            check=True,
        )
        version_line = result.stdout.splitlines()[0]

        if "g++" in version_line or "GCC" in version_line:
            import re
            m = re.search(r"(\d+)\.\d+\.\d+", version_line)
            if m and int(m.group(1)) < 14:
                sys.exit(
                    f"\n[tachyon-ipc] Unsupported GCC version: {version_line}\n"
                    f"  Required: GCC 14+\n"
                    f"  Install:  sudo apt-get install gcc-14 g++-14\n"
                    f"  Then:     CXX=g++-14 pip install tachyon-ipc\n"
                )

        if "clang" in version_line.lower():
            import re
            m = re.search(r"(\d+)\.\d+\.\d+", version_line)
            if m and int(m.group(1)) < 17:
                sys.exit(
                    f"\n[tachyon-ipc] Unsupported Clang version: {version_line}\n"
                    f"  Required: Clang 17+\n"
                    f"  Install:  sudo apt-get install clang-18\n"
                    f"  Then:     CXX=clang++-18 pip install tachyon-ipc\n"
                )

    except FileNotFoundError:
        sys.exit(
            f"\n[tachyon-ipc] C++ compiler not found: '{cxx}'\n"
            f"  tachyon-ipc compiles a C++23 extension at install time.\n"
            f"  Install GCC 14+:  sudo apt-get install gcc-14 g++-14\n"
            f"  Then:             CXX=g++-14 pip install tachyon-ipc\n"
            f"  Or Clang 17+:     sudo apt-get install clang-18\n"
            f"                    CXX=clang++-18 pip install tachyon-ipc\n"
        )
    except subprocess.CalledProcessError:
        pass


_check_compiler()

CORE_SRC_DIR = os.path.join(ROOT_DIR, "core")
CORE_LOCAL_DIR = os.path.join(HERE, "_core_local")

if os.path.exists(CORE_SRC_DIR):
    if os.path.exists(CORE_LOCAL_DIR):
        shutil.rmtree(CORE_LOCAL_DIR)
    shutil.copytree(CORE_SRC_DIR, CORE_LOCAL_DIR)

DLPACK_LOCAL_DIR = os.path.join(HERE, "_dlpack_local", "dlpack")
DLPACK_LOCAL_PATH = os.path.join(DLPACK_LOCAL_DIR, "dlpack.h")

if not os.path.exists(DLPACK_LOCAL_PATH):
    os.makedirs(DLPACK_LOCAL_DIR, exist_ok=True)
    print(f"Downloading dlpack.h → {DLPACK_LOCAL_PATH}")
    urllib.request.urlretrieve(
        "https://raw.githubusercontent.com/dmlc/dlpack/main/include/dlpack/dlpack.h",
        DLPACK_LOCAL_PATH,
    )

README_PATH = os.path.join(HERE, "README.md")
if not os.path.exists(README_PATH):
    root_readme = os.path.join(ROOT_DIR, "README.md")
    if os.path.exists(root_readme):
        shutil.copy(root_readme, README_PATH)

compile_args = [
    "-std=c++23",
    "-O3",
    "-march=native",
    "-mtune=native",
    "-flto",
    "-fno-exceptions",
    "-fno-rtti",
    "-Wall",
    "-Wextra",
]

libraries = ["rt"] if sys.platform.startswith("linux") else []

ext_modules = [
    Extension(
        name="tachyon._tachyon",
        sources=[
            "src/tachyon/_tachyon.cpp",
            "_core_local/src/arena.cpp",
            "_core_local/src/shm.cpp",
            "_core_local/src/tachyon_c.cpp",
            "_core_local/src/transport_uds.cpp",
        ],
        include_dirs=[
            "_core_local/include",
            "_dlpack_local",
        ],
        libraries=libraries,
        extra_compile_args=compile_args,
        language="c++",
    )
]

setup(
    ext_modules=ext_modules,
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    package_data={
        "tachyon": ["*.pyi", "py.typed"],
    },
    include_package_data=True,
    zip_safe=False,
)
