"""SenseThru 소개 자료 HTML -> PDF(Chrome 헤드리스). 인자 없으면 세 문서 모두, 있으면 tech/company/sales 중 그것만."""
import os, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__))
DOCS = {
    'tech': 'SenseThru_기술소개서_초안.pdf',
    'company': 'FlexFarm_회사소개서_초안.pdf',
    'sales': 'SenseThru_재배사영업자료_초안.pdf',
}
CHROME = r'C:\Program Files\Google\Chrome\Application\chrome.exe'

for name in sys.argv[1:] or DOCS:
    tmp = os.path.join(D, name + '_out.pdf')
    with tempfile.TemporaryDirectory() as prof:
        subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-first-run', '--user-data-dir=' + prof,
                        '--no-pdf-header-footer', '--virtual-time-budget=4000', '--allow-file-access-from-files',
                        '--print-to-pdf=' + tmp, 'file:///' + D.replace('\\', '/') + '/' + name + '.html'],
                       check=True, timeout=80)
    os.replace(tmp, os.path.join(D, DOCS[name]))
    print(name, 'ok')
