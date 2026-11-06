"""Run the demo's unchanged Unity service tests against production C on host.

Replace only ESP-IDF TEST_CASE registration; Unity assertions and test bodies
are unmodified. Hardware integration still requires the physical ESP32-C3.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]

def main():
    idf=os.environ.get('IDF_PATH')
    if not idf:
        description=ROOT/'build/project_description.json'
        if not description.exists():
            raise SystemExit('Set IDF_PATH or build the firmware once to locate ESP-IDF.')
        idf=json.loads(description.read_text())['idf_path']
    sdk=Path(idf)
    unity=sdk/'components/unity/unity/src'
    cjson=sdk/'components/json/cJSON'
    services=ROOT/'components/core/services'
    with tempfile.TemporaryDirectory(prefix='passport-demo-tests-') as d:
        temp=Path(d); generated=[]; functions=[]
        for test in sorted((services/'test').glob('test_*.c')):
            src=test.read_text(); n=[0]
            def replace(match):
                name=f'{test.stem}_{n[0]}';n[0]+=1
                functions.append((name,match.group(1)))
                return 'void '+name+'(void)'
            src=re.sub(r'TEST_CASE\("([^"]+)",\s*"[^"]*"\)',replace,src)
            out=temp/test.name;out.write_text(src);generated.append(out)
        runner=temp/'main.c'
        runner.write_text('#include "unity.h"\nvoid setUp(void){}\nvoid tearDown(void){}\n'+
            ''.join(f'void {f}(void);\n' for f,_ in functions)+'int main(void){UnityBegin("demo services");\n'+
            ''.join(f'UnityDefaultTestRun({f},{json.dumps(name)},__LINE__);\n' for f,name in functions)+'return UnityEnd();}\n')
        production=[services/'src'/p.name.replace('test_','') for p in generated]
        command=['cc','-std=c11','-D_DEFAULT_SOURCE','-fsanitize=undefined','-fno-sanitize-recover=all','-g',
            '-I'+str(unity),'-I'+str(cjson),'-I'+str(services/'include'),
            '-I'+str(ROOT/'components/core/ports/include'),
            *map(str,generated),*map(str,production),str(runner),str(unity/'unity.c'),str(cjson/'cJSON.c'),'-lm','-o',str(temp/'test')]
        subprocess.run(command,check=True)
        subprocess.run([str(temp/'test')],check=True)
if __name__=='__main__': main()
