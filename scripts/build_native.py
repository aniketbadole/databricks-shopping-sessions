#!/usr/bin/env python3
import pathlib, subprocess
import pyarrow
root=pathlib.Path(__file__).resolve().parents[1]
lib=pathlib.Path(pyarrow.get_library_dirs()[0])
subprocess.run(['g++','-O3','-DNDEBUG','-std=c++20','-fopenmp',str(root/'src/main.cpp'),'-I'+pyarrow.get_include(),str(next(lib.glob('libparquet.so.*'))),str(next(lib.glob('libarrow.so.*'))),'-Wl,-rpath,'+str(lib),'-Wl,-rpath,/opt/arrow','-o',str(root/'core')],check=True)
