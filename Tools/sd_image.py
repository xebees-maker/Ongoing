"""SD 카드 원본 이미지 뜨기(읽기 전용) — 2026-10-14 SD 손상 증거 보존용.

사용: python sd_image.py <물리 디스크 번호> <디스크 크기(바이트)> <출력 파일>   (관리자 권한 PowerShell)
  디스크 크기는 PowerShell `Get-Disk <번호> | Select Size`로 확인.
디스크를 처음부터 디스크 크기까지만 읽어 그대로 저장. 읽기 실패한 섹터는 0으로 채우고 위치(바이트)를 <출력>.badlist에
바로바로 기록(중간에 멈춰도 남음). 끝나면 SHA-256을 출력.
2026-10-14 — 처음 판은 디스크 끝을 지나 읽을 때 나는 오류를 "읽기 실패"로 처리해 0을 채우며 끝없이 썼음(Claude 잘못) → 크기 상한.
"""
import hashlib, sys, time

disk = int(sys.argv[1])
size = int(sys.argv[2])
out = sys.argv[3]
CHUNK = 1024 * 1024
SECTOR = 512

f = open(r'\\.\PhysicalDrive%d' % disk, 'rb', buffering=0)
h = hashlib.sha256()
bad = 0
pos = 0
t0 = time.time()
with open(out, 'wb') as o, open(out + '.badlist', 'w') as bl:
    while pos < size:
        n = min(CHUNK, size - pos)
        try:
            f.seek(pos)
            b = f.read(n)
            if len(b) != n:
                raise OSError('short read %d/%d' % (len(b), n))
        except OSError:
            buf = bytearray()
            for s in range(0, n, SECTOR):
                try:
                    f.seek(pos + s)
                    sb = f.read(SECTOR)
                    if len(sb) != SECTOR:
                        raise OSError('short')
                    buf += sb
                except OSError:
                    buf += b'\0' * SECTOR
                    bl.write('%d\n' % (pos + s))
                    bl.flush()
                    bad += 1
            b = bytes(buf)
        o.write(b)
        h.update(b)
        pos += n
        if pos % (256 * CHUNK) == 0:
            print('  %d / %d MB  %.0fs' % (pos // CHUNK, size // CHUNK, time.time() - t0), flush=True)
f.close()
print('IMAGE_OK %d bytes, bad sectors %d, %.0fs, sha256 %s' % (pos, bad, time.time() - t0, h.hexdigest()))
