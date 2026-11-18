// Smoke test for a running tracker (a container, a deployment):
//   node smoke.js ws://127.0.0.1:8000
// Announces a random torrent, scrapes it and stops. Exits 1 on failure.

const crypto = require('node:crypto')
const WebSocket = require('ws')

const url = process.argv[2] || 'ws://127.0.0.1:8000'
const infoHash = crypto.randomBytes(10).toString('hex')
const peerId = crypto.randomBytes(10).toString('hex')

const fail = (message) => {
  console.error(`FAIL ${url}: ${message}`)
  process.exit(1)
}
setTimeout(() => fail('timed out'), 10000).unref()

const socket = new WebSocket(url)
const next = (match) => new Promise((resolve) => {
  const onMessage = (data) => {
    const message = JSON.parse(data)
    if (message['failure reason']) fail(message['failure reason'])
    if (match(message)) {
      socket.off('message', onMessage)
      resolve(message)
    }
  }
  socket.on('message', onMessage)
})
const send = (message) => socket.send(JSON.stringify(message))

socket.on('error', (error) => fail(error.message))
socket.on('open', async () => {
  send({ action: 'announce', event: 'started', info_hash: infoHash, peer_id: peerId, numwant: 0, offers: [] })
  const announce = await next((m) => m.action === 'announce' && m.info_hash === infoHash)
  if (!(announce.interval > 0)) fail(`announce reply has no interval: ${JSON.stringify(announce)}`)

  send({ action: 'scrape', info_hash: infoHash })
  const scrape = await next((m) => m.action === 'scrape')
  const file = scrape.files && scrape.files[infoHash]
  if (!file || file.incomplete !== 1) fail(`scrape doesn't count the peer: ${JSON.stringify(scrape)}`)

  send({ action: 'announce', event: 'stopped', info_hash: infoHash, peer_id: peerId })
  socket.close()
  console.log(`ok ${url}: announce interval ${announce.interval} s, scrape ${JSON.stringify(file)}`)
})
