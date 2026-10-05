"""
콘 웹앱 홈 화면 아이콘(512x512 PNG) — 2026-10-05(사용자 요청: 홈 화면에 추가해 브라우저 상·하단 없이).
초록 바탕에 흰 버섯. 휴대폰이 모서리를 알아서 둥글게 깎으므로 바탕은 꽉 채움.

사용: python make_icon.py → app_icon.png
배포: curl -b <쿠키> -X POST --data-binary @app_icon.png "http://<콘>/admin/upload?file=app_icon.png"
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 512
K = 4  # 크게 그린 뒤 줄여서 테두리를 매끈하게

img = Image.new('RGB', (S * K, S * K), (22, 163, 74))
d = ImageDraw.Draw(img)
W = (255, 255, 255)
c = S * K // 2
# 갓(반원) + 줄기
cap_w, cap_h = 300 * K, 170 * K
cap_top = 120 * K
d.pieslice([c - cap_w // 2, cap_top, c + cap_w // 2, cap_top + cap_h * 2], 180, 360, fill=W)
stem_w, stem_top, stem_bot = 96 * K, cap_top + cap_h - 4 * K, 400 * K
d.rounded_rectangle([c - stem_w // 2, stem_top, c + stem_w // 2, stem_bot], radius=36 * K, fill=W)
img.resize((S, S), Image.LANCZOS).save(os.path.join(HERE, 'app_icon.png'), optimize=True)
print('app_icon.png')
