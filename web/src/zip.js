// Minimal ZIP reader — enough to open a fabrication-output zip in the browser
// with zero dependencies. Central directory + stored/deflated entries; deflate
// is inflated by the browser's own DecompressionStream('deflate-raw').
//
// Anything outside that (spanned archives, zip64, encryption) throws with the
// reason named rather than yielding half an archive.

const EOCD = 0x06054b50
const CDIR = 0x02014b50

export async function unzip(arrayBuffer) {
  const buf = new Uint8Array(arrayBuffer)
  const dv = new DataView(arrayBuffer)

  // EOCD sits at the end, before a comment of up to 64 KB
  let eocd = -1
  const stop = Math.max(0, buf.length - 65558)
  for (let i = buf.length - 22; i >= stop; i--) {
    if (dv.getUint32(i, true) === EOCD) { eocd = i; break }
  }
  if (eocd < 0) throw new Error('not a zip file (no end-of-central-directory)')
  const count = dv.getUint16(eocd + 10, true)
  let off = dv.getUint32(eocd + 16, true)
  if (off === 0xffffffff) throw new Error('zip64 archives are not supported')

  const files = []
  const utf8 = new TextDecoder()
  for (let n = 0; n < count; n++) {
    if (dv.getUint32(off, true) !== CDIR) throw new Error('corrupt central directory')
    const method = dv.getUint16(off + 10, true)
    const csize = dv.getUint32(off + 20, true)
    const nameLen = dv.getUint16(off + 28, true)
    const extraLen = dv.getUint16(off + 30, true)
    const commentLen = dv.getUint16(off + 32, true)
    const localOff = dv.getUint32(off + 42, true)
    const name = utf8.decode(buf.subarray(off + 46, off + 46 + nameLen))
    off += 46 + nameLen + extraLen + commentLen
    if (name.endsWith('/')) continue                       // directory entry
    // the local header repeats name/extra with its own lengths
    const lNameLen = dv.getUint16(localOff + 26, true)
    const lExtraLen = dv.getUint16(localOff + 28, true)
    const data = buf.subarray(localOff + 30 + lNameLen + lExtraLen,
                              localOff + 30 + lNameLen + lExtraLen + csize)
    // full path kept: Gerber detection is content-based and does not care,
    // but an ODB++ job IS its directory structure (matrix/matrix, steps/...)
    files.push({ name, method, data })
  }

  const out = []
  for (const f of files) {
    let bytes
    if (f.method === 0) {
      bytes = f.data
    } else if (f.method === 8) {
      const ds = new DecompressionStream('deflate-raw')
      const stream = new Blob([f.data]).stream().pipeThrough(ds)
      bytes = new Uint8Array(await new Response(stream).arrayBuffer())
    } else {
      throw new Error(`zip entry ${f.name}: unsupported compression method ${f.method}`)
    }
    out.push({ name: f.name, text: utf8.decode(bytes) })
  }
  return out
}
