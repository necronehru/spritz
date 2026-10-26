const fs = require('fs')
const p = 'node_modules/bittorrent-tracker/lib/client/http-tracker.js'
let s = fs.readFileSync(p, 'utf8')

const old = "this.client.emit('peer', `${peer.ip}:${peer.port}`)"
const replacement = [
  "const ip = typeof peer.ip === 'string'",
  '  ? peer.ip',
  "  : Buffer.from(peer.ip).toString('utf8')",
  "this.client.emit('peer', `${ip}:${peer.port}`)"
].join('\n')

if (!s.includes(old)) {
  if (s.includes(replacement)) {
    console.log('Torrent peer-IP fix already applied.')
    process.exit(0)
  }
  throw new Error('Could not find expected tracker code; refusing to modify it.')
}

const count = s.split(old).length - 1
s = s.replaceAll(old, replacement)
fs.writeFileSync(p, s)
console.log(`Torrent peer-IP fix applied to ${count} location(s).`)
