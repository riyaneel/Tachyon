import os
import shutil
import sys
from setuptools import setup, Extension, find_packages

HERE = os.path.abspath(os.path.dirname(__file__))
ROOT_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))

CORE_SRC_DIR = os.path.join(ROOT_DIR, "core")
CORE_LOCAL_DIR = os.path.join(HERE, "_core_local")

if os.path.exists(CORE_SRC_DIR):
    if os.path.exists(CORE_LOCAL_DIR):
        shutil.rmtree(CORE_LOCAL_DIR)
    shutil.copytree(CORE_SRC_DIR, CORE_LOCAL_DIR)

compile_args = [
    "-std=c++26",
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
            "_core_local/src/transport_uds.cpp"
        ],
        include_dirs=["_core_local/include"],
        libraries=libraries,
        extra_compile_args=compile_args,
        language="c++"
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
