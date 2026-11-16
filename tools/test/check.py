"""Checks the PC build of the tools (tools/test/run.sh): hashes against hashlib/zlib, zip
extraction against what was put in the archive."""
import hashlib, os, random, subprocess, sys, zipfile, zlib
W = sys.argv[1]
T = os.path.join(W, 't')
fails = 0

def check(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what)
    fails += not cond

rnd = random.Random(1)
def blob(n, kind):
    if kind == 'zero': return bytes(n)
    if kind == 'rand': return rnd.randbytes(n)
    words = [b'retro', b'go', b'game', b'watch', b'zelda', b'mario', b'\n', b' ']
    out = bytearray()
    while len(out) < n: out += rnd.choice(words)
    return bytes(out[:n])

# hashes, at sizes around the 64-byte block edges
for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 4093, 100000):
    p = os.path.join(W, 'h.bin'); d = blob(n, 'rand'); open(p, 'wb').write(d)
    got = subprocess.run([T, 'hash', p], capture_output=True, text=True).stdout.split()
    want = ['%08x' % zlib.crc32(d), hashlib.md5(d).hexdigest(), hashlib.sha1(d).hexdigest()]
    check(got == want, 'hash %d bytes' % n)

# zip: stored and deflated, folders, UTF-8 and DOS names, nasty paths
files = {
    'a.txt': blob(1000, 'text'), 'dir/b.bin': blob(300000, 'rand'), 'dir/sub/c.txt': blob(200000, 'text'),
    'zero.bin': blob(1 << 20, 'zero'), 'empty.txt': b'', 'ñandú/canción.txt': blob(5000, 'text'),
    '../escape.txt': b'nope', '/abs.txt': b'abs', 'bad:name?.txt': b'bad',
}
want = {'a.txt', 'dir/b.bin', 'dir/sub/c.txt', 'zero.bin', 'empty.txt', 'ñandú/canción.txt', 'escape.txt', 'abs.txt', 'bad_name_.txt'}
for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
    z = os.path.join(W, 'z%d.zip' % method)
    with zipfile.ZipFile(z, 'w', method) as f:
        f.writestr('emptydir/', b'')
        for k, v in files.items(): f.writestr(k, v)
    out = os.path.join(W, 'out%d' % method)
    r = subprocess.run([T, 'unzip', z, out], capture_output=True, text=True)
    print(r.stdout.strip())
    got = set()
    for root, dirs, fs in os.walk(out):
        for x in fs: got.add(os.path.relpath(os.path.join(root, x), out))
    check(got == want, 'zip method %d: names' % method)
    same = all(open(os.path.join(out, k.lstrip('/').replace('../', '').replace(':', '_').replace('?', '_')), 'rb').read() == v for k, v in files.items())
    check(same, 'zip method %d: contents' % method)
    check(os.path.isdir(os.path.join(out, 'emptydir')), 'zip method %d: empty folder' % method)
    # again: everything exists; answer "skip" (3)
    r = subprocess.run([T, 'unzip', z, out, '3'], capture_output=True, text=True)
    check('skipped 9' in r.stdout, 'zip method %d: skip existing' % method)

# a DOS (CP437) name without the UTF-8 flag
z = os.path.join(W, 'dos.zip')
with zipfile.ZipFile(z, 'w') as f: f.writestr('CANCIXN.TXT', b'hola')
b = open(z, 'rb').read().replace(b'CANCIXN', b'CANCI\xa2N'); open(z, 'wb').write(b)   # 0xA2 = o-acute in CP437
out = os.path.join(W, 'outdos')
subprocess.run([T, 'unzip', z, out], capture_output=True)
check(os.listdir(out) == ['CANCIóN.TXT'], 'zip DOS name -> %s' % os.listdir(out))

# damaged data: CRC mismatch is reported, not silently written as good
z = os.path.join(W, 'z8.zip'); b = bytearray(open(z, 'rb').read())
i = b.find(b'retro'); 
bad = os.path.join(W, 'bad.zip')
with zipfile.ZipFile(bad, 'w', zipfile.ZIP_STORED) as f: f.writestr('x.txt', b'hello world' * 100)
b = bytearray(open(bad, 'rb').read()); b[100] ^= 0xFF; open(bad, 'wb').write(b)
r = subprocess.run([T, 'unzip', bad, os.path.join(W, 'outbad')], capture_output=True, text=True)
check('bad 1' in r.stdout, 'zip damaged entry reported: ' + r.stdout.strip())
r = subprocess.run([T, 'unzip', os.path.join(W, 'h.bin'), os.path.join(W, 'outnot')], capture_output=True, text=True)
check(r.returncode == 2, 'not a zip: ' + r.stdout.strip())
print('FAILURES: %d' % fails)
sys.exit(1 if fails else 0)
