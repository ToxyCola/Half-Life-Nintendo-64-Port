import os
import sys
import shutil
import urllib.request
import zipfile
import glob
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(REPO, 'builder', 'out', 'work')
STAGE = os.path.join(WORK, 'stage')
OUT = os.path.join(REPO, 'HL64Builder.exe')
DL = os.path.join(WORK, 'dl')

os.makedirs(os.path.join(STAGE, 'engine'), exist_ok=True)
os.makedirs(os.path.join(STAGE, 'tools'), exist_ok=True)
os.makedirs(os.path.join(STAGE, 'hl64', 'tools'), exist_ok=True)
os.makedirs(os.path.join(STAGE, 'hl64', 'src'), exist_ok=True)
os.makedirs(os.path.join(STAGE, 'hl64', 'assets'), exist_ok=True)
os.makedirs(os.path.join(STAGE, 'licenses'), exist_ok=True)
os.makedirs(DL, exist_ok=True)

print("== 1. Copying engine files")
shutil.copy2(os.path.join(REPO, 'build', 'hl64.elf.stripped'), os.path.join(STAGE, 'engine'))
shutil.copy2(os.path.join(REPO, 'build', 'hl64.elf.sym'), os.path.join(STAGE, 'engine'))
shutil.copy2(r'C:\n64-toolchain\mips64-elf\include\libdragon.version', os.path.join(STAGE, 'engine'))

print("== 2. Copying tools")
for t in ['mksprite.exe', 'mkasset.exe', 'audioconv64.exe', 'mkdfs.exe', 'n64tool.exe', 'ed64romconfig.exe']:
    shutil.copy2(os.path.join(r'C:\n64-toolchain\bin', t), os.path.join(STAGE, 'tools'))
shutil.copy2(os.path.join(REPO, 'tools', 'navgen.exe'), os.path.join(STAGE, 'tools'))

print("== 3. Copying converters and assets")
for py in glob.glob(os.path.join(REPO, 'tools', '*.py')):
    shutil.copy2(py, os.path.join(STAGE, 'hl64', 'tools'))
shutil.copy2(os.path.join(REPO, 'src', 'npc.c'), os.path.join(STAGE, 'hl64', 'src'))
for asset_f in glob.glob(os.path.join(REPO, 'assets', '*.*')):
    shutil.copy2(asset_f, os.path.join(STAGE, 'hl64', 'assets'))

shutil.copy2(r'C:\msys64\home\dylan\libdragon\LICENSE.md', os.path.join(STAGE, 'licenses', 'libdragon-LICENSE.md'))
shutil.copy2(os.path.join(REPO, 'valve_sdk', 'LICENSE'), os.path.join(STAGE, 'licenses', 'valve-sdk-LICENSE'))
shutil.copy2(os.path.join(REPO, 'README.md'), os.path.join(STAGE, 'licenses', 'HL64-README.md'))

print("== 4. Setting up embeddable Python")
py_ver = "3.12.10"
py_zip = os.path.join(DL, f"python-{py_ver}.zip")
if not os.path.exists(py_zip):
    print(f"Downloading Python {py_ver} embeddable...")
    urllib.request.urlretrieve(f"https://www.python.org/ftp/python/{py_ver}/python-{py_ver}-embed-amd64.zip", py_zip)

py_stage = os.path.join(STAGE, 'python')
os.makedirs(os.path.join(py_stage, 'Lib', 'site-packages'), exist_ok=True)
with zipfile.ZipFile(py_zip, 'r') as zf:
    zf.extractall(py_stage)

wheel_dir = os.path.join(DL, 'wheels')
os.makedirs(wheel_dir, exist_ok=True)
print("Downloading numpy and Pillow wheels...")
subprocess.run([
    sys.executable, '-m', 'pip', 'download', '-q',
    '--only-binary=:all:', '--platform', 'win_amd64', '--python-version', '3.12', '--implementation', 'cp',
    'numpy==1.26.4', 'pillow==10.4.0', '-d', wheel_dir
], check=True)

for whl in glob.glob(os.path.join(wheel_dir, '*.whl')):
    with zipfile.ZipFile(whl, 'r') as zf:
        zf.extractall(os.path.join(py_stage, 'Lib', 'site-packages'))

pth_files = glob.glob(os.path.join(py_stage, 'python3*._pth'))
if pth_files:
    zip_files = glob.glob(os.path.join(py_stage, 'python3*.zip'))
    zip_name = os.path.basename(zip_files[0]) if zip_files else "python312.zip"
    with open(pth_files[0], 'w', encoding='utf-8') as f:
        f.write(f"{zip_name}\n.\nLib\\site-packages\n..\\hl64\\tools\nimport site\n")

stub_exe = os.path.join(REPO, 'builder', 'stub.exe')
if not os.path.exists(stub_exe):
    print("Building stub.exe...")
    cmd = "export MSYSTEM=MINGW64; . /etc/profile; cd /i/Half-Life-Nintendo-64-Port; gcc -O2 -s -o builder/stub.exe builder/hl64builder.c -lz -ladvapi32 -lshell32"
    subprocess.run([r'C:\msys64\usr\bin\bash.exe', '-lc', cmd], check=True)

print("== 5. Packing HL64Builder.exe")
subprocess.run([sys.executable, os.path.join(REPO, 'builder', 'pack.py'), stub_exe, STAGE, OUT], check=True)
print(f"Successfully generated: {OUT}")

