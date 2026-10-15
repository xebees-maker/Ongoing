"""SD 이미지의 FAT32 구조 분석(읽기 전용) — 2026-10-14 콘 SD 손상 분석.

사용: python fat_inspect.py <이미지> [파티션 시작 섹터 — MBR이 망가졌을 때]
부트 섹터·FSInfo 점검, FAT1/FAT2 비교, 루트에서 stats·photos 폴더를 따라가며 디렉터리 항목 상태(깨진 이름 등),
파일별 클러스터 체인 이상(범위 밖·빈 클러스터·다른 파일과 겹침), 통계 집계 파일(a1/a2/a3)의 0xFF 구간이 어느 클러스터인지.
"""
import struct, sys, collections

img = open(sys.argv[1], 'rb')


def rd(off, n):
    img.seek(off)
    return img.read(n)


mbr = rd(0, 512)
p_type = mbr[446 + 4]
p_lba = struct.unpack_from('<I', mbr, 446 + 8)[0]
p_cnt = struct.unpack_from('<I', mbr, 446 + 12)[0]
if len(sys.argv) > 2:   # 2026-10-15 — MBR이 망가진 이미지: 파티션 시작 섹터를 직접 줌
    p_lba = int(sys.argv[2])
base = p_lba * 512
print('MBR sig %s, part type 0x%02X lba %d count %d' % (mbr[510:512].hex(), p_type, p_lba, p_cnt))

bs = rd(base, 512)
bps, spc, rsv, nfat = struct.unpack_from('<HBHB', bs, 11)
tot32 = struct.unpack_from('<I', bs, 32)[0]
fatsz = struct.unpack_from('<I', bs, 36)[0]
extflags = struct.unpack_from('<H', bs, 40)[0]
rootclus = struct.unpack_from('<I', bs, 44)[0]
fsinfo = struct.unpack_from('<H', bs, 48)[0]
bkboot = struct.unpack_from('<H', bs, 50)[0]
print('BOOT jmp %s oem %r bps %d spc %d rsv %d nfat %d tot %d fatsz %d extflags 0x%04X root %d fsinfo %d bk %d sig %s fstype %r' % (
    bs[0:3].hex(), bs[3:11], bps, spc, rsv, nfat, tot32, fatsz, extflags, rootclus, fsinfo, bkboot, bs[510:512].hex(), bs[82:90]))
bk = rd(base + bkboot * bps, 512)
print('backup boot sector identical:', bk == bs)
fi = rd(base + fsinfo * bps, 512)
print('FSINFO sigs %s %s %s free %d next %d' % (fi[0:4].hex(), fi[484:488].hex(), fi[508:512].hex(),
                                                 struct.unpack_from('<I', fi, 488)[0], struct.unpack_from('<I', fi, 492)[0]))

csize = bps * spc
fat_off = base + rsv * bps
data_off = fat_off + nfat * fatsz * bps
nclus = (tot32 - rsv - nfat * fatsz) // spc
fat1 = rd(fat_off, fatsz * bps)
fat2 = rd(fat_off + fatsz * bps, fatsz * bps) if nfat > 1 else fat1
F1 = struct.unpack('<%dI' % (len(fat1) // 4), fat1)
F2 = struct.unpack('<%dI' % (len(fat2) // 4), fat2)
print('cluster %d bytes, clusters %d, data at %d' % (csize, nclus, data_off))
d = [i for i in range(2, nclus + 2) if (F1[i] & 0x0FFFFFFF) != (F2[i] & 0x0FFFFFFF)]
print('FAT1 vs FAT2 differing entries:', len(d), d[:20])
used = sum(1 for i in range(2, nclus + 2) if F1[i] & 0x0FFFFFFF)
print('FAT1 used clusters %d (%.1f MB)' % (used, used * csize / 1e6))


def coff(c):
    return data_off + (c - 2) * csize


def chain(c, F=F1, limit=2000000):
    out = []
    seen = set()
    while 2 <= c < nclus + 2 and c not in seen and len(out) < limit:
        out.append(c)
        seen.add(c)
        n = F[c] & 0x0FFFFFFF
        if n >= 0x0FFFFFF8:
            return out, 'eoc'
        if n == 0:
            return out, 'free-link'
        c = n
    if c in seen:
        return out, 'loop'
    return out, 'bad-link %d' % c


def read_dir(c):
    cl, st = chain(c)
    data = b''.join(rd(coff(x), csize) for x in cl)
    ents = []
    lfn = []
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            lfn = []
            continue
        attr = e[11]
        if attr == 0x0F:
            part = e[1:11] + e[14:26] + e[28:32]
            lfn.insert(0, part.decode('utf-16le', 'replace').split('\x00')[0])
            continue
        short = e[0:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else '')
        name = ''.join(lfn) or short
        lfn = []
        clus = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        size = struct.unpack_from('<I', e, 28)[0]
        ents.append((name, attr, clus, size, short))
    return ents, cl, st


def ok_name(n):
    return all(32 <= ord(ch) < 127 for ch in n)


root, rcl, rst = read_dir(rootclus)
print('\nROOT chain %d clusters (%s):' % (len(rcl), rst), [(n, hex(a), c, s) for n, a, c, s, _ in root])
owner = collections.defaultdict(list)
for x in rcl:
    owner[x].append('/')


def walk(path, c, depth=0):
    ents, cl, st = read_dir(c)
    for x in cl:
        owner[x].append(path + '/')
    bad = [e for e in ents if not ok_name(e[0])]
    print('\nDIR %s: chain %d clusters (%s), entries %d, malformed names %d' % (path, len(cl), st, len(ents), len(bad)))
    for e in bad[:5]:
        print('   malformed:', repr(e[0])[:40], 'attr', hex(e[1]), 'clus', e[2], 'size', e[3])
    files = []
    for name, attr, clus, size, short in ents:
        if name in ('.', '..') or not ok_name(name):
            continue
        if attr & 0x10:
            if depth < 3:
                walk(path + '/' + name, clus, depth + 1)
            continue
        fc, fst = chain(clus) if clus else ([], 'empty')
        for x in fc:
            owner[x].append(path + '/' + name)
        need = (size + csize - 1) // csize
        files.append((name, size, len(fc), need, fst, fc[:1]))
    probs = [f for f in files if f[4] != 'eoc' and f[1] > 0 or (f[1] > 0 and f[2] != f[3])]
    print('   files %d, chain problems %d' % (len(files), len(probs)))
    for f in probs[:10]:
        print('   chain problem:', f[0], 'size', f[1], 'chain', f[2], 'need', f[3], f[4])
    return files


for name, attr, clus, size, short in root:
    if attr & 0x10 and name not in ('.', '..') and ok_name(name):
        walk('/' + name, clus)

cross = {c: o for c, o in owner.items() if len(o) > 1}
print('\nclusters claimed by more than one file/dir: %d' % len(cross))
for c, o in list(cross.items())[:15]:
    print('   cluster %d:' % c, o)

# 통계 집계 파일 a1~a3 의 0xFF 구간이 어느 클러스터에 있는지
print()
for path in [p for c, os_ in owner.items() for p in os_]:
    pass
seen_files = sorted(set(p for os_ in owner.values() for p in os_ if '/stats/a' in p))
for p in seen_files:
    cls = sorted([c for c, os_ in owner.items() if p in os_])
    ff = []
    for c in cls:
        blk = rd(coff(c), csize)
        if blk.count(0xFF) > csize * 0.9:
            ff.append(c)
    print('%s: clusters %d, mostly-0xFF clusters %d %s' % (p, len(cls), len(ff), ff[:10]))
