"""SenseThru 소개 자료 HTML -> PDF(Chrome 헤드리스). 인자 없으면 세 문서 모두, 있으면 tech/company/sales 중 그것만.

<i data-ic="이름" data-s="크기" data-c="색" data-w="선 굵기"></i> 는 icons/이름.svg(Lucide)로 바꿔 넣고 인쇄한다.
"""
import os, re, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__))
DOCS = {
    'tech': 'SenseThru_기술소개서_초안.pdf',
    'company': 'FlexFarm_회사소개서_초안.pdf',
    'sales': 'FlexFarm_재배사영업자료_초안.pdf',
    'covers': 'SenseThru_표지시안.pdf',
    'art': 'SenseThru_그림시안.pdf',
}
CHROME = r'C:\Program Files\Google\Chrome\Application\chrome.exe'


def icon(m):
    a = dict(re.findall(r'data-(\w+)="([^"]*)"', m.group(0)))
    with open(os.path.join(D, 'icons', a['ic'] + '.svg'), encoding='utf-8') as f:
        s = re.sub(r'<!--.*?-->', '', f.read(), flags=re.S).strip()
    size = a.get('s', '24')
    s = re.sub(r'class="[^"]*"', 'class="ic"', s)
    s = s.replace('width="24"', f'width="{size}"').replace('height="24"', f'height="{size}"')
    s = s.replace('stroke="currentColor"', f'stroke="{a.get("c", "currentColor")}"')
    s = s.replace('stroke-width="2"', f'stroke-width="{a.get("w", "1.8")}"')
    return re.sub(r'\s+', ' ', s)


for name in sys.argv[1:] or DOCS:
    with open(os.path.join(D, name + '.html'), encoding='utf-8') as f:
        html = re.sub(r'<i data-ic="[^"]*"[^>]*></i>', icon, f.read())
    page = os.path.join(D, f'_render_{name}.html')
    with open(page, 'w', encoding='utf-8') as f:
        f.write(html)
    tmp = os.path.join(D, name + '_out.pdf')
    try:
        with tempfile.TemporaryDirectory() as prof:
            subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-first-run', '--user-data-dir=' + prof,
                            '--no-pdf-header-footer', '--virtual-time-budget=6000', '--allow-file-access-from-files',
                            '--print-to-pdf=' + tmp, 'file:///' + page.replace('\\', '/')],
                           check=True, timeout=90)
    finally:
        os.remove(page)
    os.replace(tmp, os.path.join(D, DOCS[name]))
    print(name, 'ok')
