"""SD 이미지에서 손상 구간이 어디서 온 데이터인지 추적(읽기 전용) — 2026-10-15 MPIO 카드 분석.

사용: python fat_trace.py <이미지> <파티션 시작 섹터>
FAT32를 따라 모든 파일의 클러스터 지도를 만들고,
 1) 맨 앞 16섹터, 2) /PHOTOS/<캠> 디렉터리 클러스터의 무작위 섹터, 3) /STATS 원시 기록 파일의 비정상 레코드 구간 —
 각각의 내용(512바이트 섹터 단위)이 이미지의 다른 곳(다른 파일)에도 있는지 찾아, 있으면 그 파일·파일 안 위치를 출력.
"""
import mmap, struct, sys, collections

f = open(sys.argv[1], 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
base = int(sys.argv[2]) * 512
bs = m[base:base + 512]
bps, spc, rsv, nfat = struct.unpack_from('<HBHB', bs, 11)
fatsz = struct.unpack_from('<I', bs, 36)[0]
rootclus = struct.unpack_from('<I', bs, 44)[0]
fat_off = base + rsv * bps
data_off = fat_off + nfat * fatsz * bps
csz = spc * bps
nclus = (len(m) - data_off) // csz


def fat(c):
    return struct.unpack_from('<I', m, fat_off + 4 * c)[0] & 0x0FFFFFFF


def chain(c):
    out = []
    seen = set()
    while 2 <= c < 0x0FFFFFF8 and c not in seen and len(out) < 200000:
        seen.add(c)
        out.append(c)
        c = fat(c)
    return out


def coff(c):
    return data_off + (c - 2) * csz


def entries(c):
    for cl in chain(c):
        d = m[coff(cl):coff(cl) + csz]
        for i in range(0, csz, 32):
            e = d[i:i + 32]
            if e[0] == 0:
                return
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            name = e[0:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else '')
            clus = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            yield name, e[11], clus, struct.unpack_from('<I', e, 28)[0], coff(cl) + i


# 파일 지도: 클러스터 -> (경로, 파일 안 클러스터 순번)
owner = {}
files = {}


def walk(c, path, depth=0):
    for name, attr, clus, size, eoff in entries(c):
        if name in ('.', '..') or attr & 0x08:
            continue
        p = path + '/' + name
        if not (2 <= clus < nclus + 2):
            continue
        ch = chain(clus)
        files[p] = (attr, clus, size, ch)
        for k, cl in enumerate(ch):
            owner.setdefault(cl, []).append((p, k))
        if attr & 0x10 and depth < 3:
            walk(clus, p, depth + 1)


walk(rootclus, '')
for k, cl in enumerate(chain(rootclus)):
    owner.setdefault(cl, []).append(('/', k))
print('files', len(files), 'cluster size', csz, 'data at', data_off)


def where(off):
    """이미지 바이트 위치 → 설명"""
    if off < data_off:
        return 'sector %d (before data area)' % (off // 512)
    cl = (off - data_off) // csz + 2
    o = owner.get(cl)
    inoff = (off - data_off) % csz
    if not o:
        return 'cluster %d (+%d) FREE/unowned' % (cl, inoff)
    return 'cluster %d (+%d) %s' % (cl, inoff, ', '.join('%s #%d -> file offset %d' % (p, k, k * csz + inoff) for p, k in o))


def find_copies(off, n=512, label=''):
    blk = m[off:off + n]
    if len(set(blk)) < 8:
        print('  %s: low-entropy (%s)' % (label, blk[:8].hex()))
        return
    hits = []
    pos = m.find(blk[:64])
    while pos >= 0 and len(hits) < 5:
        if pos != off and m[pos:pos + n] == blk:
            hits.append(pos)
        pos = m.find(blk[:64], pos + 1)
    print('  %s @%s: %s' % (label, where(off), ('COPY at ' + '; '.join(where(h) for h in hits)) if hits else 'no other copy'))


print('\n== 1) first 16 sectors')
for s in range(16):
    find_copies(s * 512, 512, 'sector %d' % s)

print('\n== 2) photo directory random sectors')
for p, (attr, clus, size, ch) in files.items():
    if attr & 0x10 and p.startswith('/PHOTOS/'):
        for k, cl in enumerate(ch):
            for s in range(spc):
                off = coff(cl) + s * 512
                blk = m[off:off + 512]
                bad = 0
                for i in range(0, 512, 32):
                    e = blk[i:i + 32]
                    if e[0] not in (0, 0xE5) and e[11] != 0x0F and not all(32 <= ch_ < 127 for ch_ in e[0:11]):
                        bad += 1
                if bad >= 8:
                    find_copies(off, 512, '%s dir cluster #%d sector %d' % (p, k, s))

print('\n== 3) stats raw files: bad record runs')
REC = 17
for p, (attr, clus, size, ch) in sorted(files.items()):
    if not (p.startswith('/STATS/R') and not attr & 0x10):
        continue
    data = b''.join(m[coff(cl):coff(cl) + csz] for cl in ch)[:size]
    if len(data) < size:
        print(p, 'CHAIN SHORT: size', size, 'but chain holds', len(data))
    n = len(data) // REC
    bad = [i for i in range(n) if not (1791000000 <= struct.unpack_from('<I', data, i * REC)[0] <= 1792200000
                                        and data[i * REC + 11] in (1, 2, 3, 4, 5))]
    print(p, 'size', size, 'records', n, 'bad', len(bad), 'clusters', len(ch))
    if not bad:
        continue
    runs = []
    s0 = bad[0]; prev = bad[0]
    for i in bad[1:] + [None]:
        if i is None or i != prev + 1:
            runs.append((s0, prev)); s0 = i
        prev = i if i is not None else prev
    for a, b in runs[:20]:
        ba, bb = a * REC, (b + 1) * REC
        print('  bad records %d..%d = file bytes %d..%d (clusters #%d..#%d, +%d..+%d)' % (a, b, ba, bb, ba // csz, (bb - 1) // csz, ba % csz, (bb - 1) % csz))
        t_before = struct.unpack_from('<I', data, (a - 1) * REC)[0] if a > 0 else None
        t_after = struct.unpack_from('<I', data, (b + 1) * REC)[0] if b + 1 < n else None
        print('    neighbours t', t_before, t_after)
        first_sector = (ba // 512 + 1) * 512   # 손상 구간 안에 통째로 들어가는 첫 섹터
        for so in range(first_sector, min(bb - 512, first_sector + 512 * 64), 512 * 16):
            k = so // csz
            find_copies(coff(ch[k]) + so % csz, 512, 'file byte %d' % so)
