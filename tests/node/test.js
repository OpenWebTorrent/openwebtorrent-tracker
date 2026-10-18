// Integration tests: spawns the tracker binary and talks to it over websockets.
// TRACKER_BIN overrides the binary path (default: ../../build/openwebtorrent-tracker).

const { test, before, after } = require('node:test')
const assert = require('node:assert')
const { spawn } = require('node:child_process')
const path = require('node:path')
const WebSocket = require('ws')

const BIN = process.env.TRACKER_BIN || path.join(__dirname, '../../build/openwebtorrent-tracker')
const PORT = 20000 + Math.floor(Math.random() * 20000)
const URL = `ws://127.0.0.1:${PORT}`
// Second tracker with tight limits
const LIMITED_PORT = PORT + 1
const LIMITED_URL = `ws://127.0.0.1:${LIMITED_PORT}`

const INFO_HASH = 'i'.repeat(20)
const id = (c) => c.repeat(20)
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

const trackers = []

function startTracker (port, args = []) {
  const tracker = spawn(BIN, ['--no-ssl', '--host', '127.0.0.1', '--port', String(port), ...args], { stdio: ['ignore', 'pipe', 'inherit'] })
  trackers.push(tracker)
  return new Promise((resolve, reject) => {
    tracker.stdout.on('data', (d) => { if (d.toString().includes('Listening')) resolve() })
    tracker.on('exit', (code) => reject(new Error(`tracker exited with ${code}`)))
  })
}

before(() => Promise.all([
  startTracker(PORT),
  startTracker(LIMITED_PORT, ['--rate-limit', '5', '--rate-burst', '10', '--max-connections-per-ip', '2', '--trust-proxy'])
]))

after(() => trackers.forEach((t) => t.kill()))

function connect (url = URL, headers = {}) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url, { headers })
    socket.messages = []
    socket.waiters = []
    socket.on('message', (data) => {
      const msg = JSON.parse(data)
      socket.messages.push(msg)
      socket.waiters = socket.waiters.filter((w) => !(w.match(msg) && (w.resolve(msg), true)))
    })
    socket.next = (match = () => true, timeout = 2000) => new Promise((resolve, reject) => {
      const found = socket.messages.find(match)
      if (found) return resolve(found)
      socket.waiters.push({ match, resolve })
      setTimeout(() => reject(new Error('timeout waiting for message')), timeout)
    })
    socket.json = (obj) => socket.send(JSON.stringify(obj))
    socket.on('open', () => resolve(socket))
    socket.on('error', reject)
  })
}

const offers = (n) => Array.from({ length: n }, (_, i) => ({ offer_id: `o${i}`, offer: { type: 'offer', sdp: `sdp${i}` } }))

test('offer/answer exchange between two peers', async () => {
  const a = await connect()
  const b = await connect()

  a.json({ action: 'announce', event: 'started', info_hash: INFO_HASH, peer_id: id('a'), numwant: 5, offers: offers(5) })
  const res = await a.next((m) => m.interval)
  assert.strictEqual(res.info_hash, INFO_HASH)
  assert.strictEqual(res.incomplete, 1)

  b.json({ action: 'announce', event: 'started', info_hash: INFO_HASH, peer_id: id('b'), numwant: 5, offers: offers(5) })
  const offer = await a.next((m) => m.offer)
  assert.strictEqual(offer.peer_id, id('b'))
  assert.strictEqual(offer.offer.sdp, 'sdp0')

  a.json({ action: 'announce', info_hash: INFO_HASH, peer_id: id('a'), to_peer_id: id('b'), offer_id: offer.offer_id, answer: { type: 'answer', sdp: 'ans' } })
  const answer = await b.next((m) => m.answer)
  assert.deepStrictEqual(answer, { action: 'announce', answer: { type: 'answer', sdp: 'ans' }, offer_id: 'o0', peer_id: id('a'), info_hash: INFO_HASH })

  a.close(); b.close()
})

test('malformed messages do not crash the tracker', async () => {
  const s = await connect()
  s.json({ action: 5 })
  const failure = await s.next((m) => m['failure reason'])
  assert.strictEqual(failure.action, 'announce')

  s.json({ action: 'announce', event: 'started', info_hash: INFO_HASH, peer_id: 5 })
  const withHash = await s.next((m) => m['failure reason'] && m.info_hash)
  assert.strictEqual(withHash.info_hash, INFO_HASH)

  for (const msg of [[], 'x', { action: 'announce', event: {} }, { action: 'scrape', info_hash: {} }]) s.json(msg)
  s.send('['.repeat(30000) + ']'.repeat(30000))
  s.send(Buffer.from([0xff, 0x00, 0x01]))
  await wait(200)

  // Still serving new clients
  const t = await connect()
  t.json({ action: 'scrape', info_hash: INFO_HASH })
  assert.ok((await t.next((m) => m.action === 'scrape')).files[INFO_HASH])
  t.close()
})

test('reconnecting with the same peer_id keeps the new connection reachable', async () => {
  const ih = 'r'.repeat(20)
  const a1 = await connect()
  a1.json({ action: 'announce', event: 'started', info_hash: ih, peer_id: id('A'), numwant: 0, offers: [] })
  await a1.next((m) => m.interval)

  const a2 = await connect()
  a2.json({ action: 'announce', event: 'started', info_hash: ih, peer_id: id('A'), numwant: 0, offers: [] })
  await a2.next((m) => m.interval)
  a1.close()
  await wait(100)

  const b = await connect()
  b.json({ action: 'announce', event: 'started', info_hash: ih, peer_id: id('B'), numwant: 0, offers: [] })
  await b.next((m) => m.interval)
  b.json({ action: 'announce', info_hash: ih, peer_id: id('B'), to_peer_id: id('A'), offer_id: 'x', answer: { type: 'answer', sdp: 'y' } })
  const answer = await a2.next((m) => m.answer)
  assert.strictEqual(answer.peer_id, id('B'))
  assert.strictEqual(b.readyState, WebSocket.OPEN)

  a2.close(); b.close()
})

test('stats and 404', async () => {
  const s = await connect()
  s.json({ action: 'announce', event: 'started', info_hash: 's'.repeat(20), peer_id: id('s'), offers: [] })
  await s.next((m) => m.interval)

  const stats = await (await fetch(`http://127.0.0.1:${PORT}/stats.json`)).json()
  assert.ok(stats.torrentsCount >= 1)
  assert.ok(stats.peersCount >= 1)
  assert.ok(stats.memory.rss > 1024 * 1024)
  assert.strictEqual(stats.servers[0].server, `127.0.0.1:${PORT}`)

  assert.strictEqual((await fetch(`http://127.0.0.1:${PORT}/nope`)).status, 404)
  s.close()
})

test('full scrape is disabled by default', async () => {
  const s = await connect()
  s.json({ action: 'scrape' })
  const failure = await s.next((m) => m.action === 'scrape')
  assert.strictEqual(failure['failure reason'], 'scrape: full scrape is disabled')
  s.close()
})

test('flooding a connection gets it closed', async () => {
  const s = await connect(LIMITED_URL, { 'X-Forwarded-For': '10.0.0.1' })
  const closed = new Promise((resolve) => s.on('close', (code) => resolve(code)))
  for (let i = 0; i < 30; i++) s.json({ action: 'scrape', info_hash: INFO_HASH })
  assert.strictEqual(await closed, 1008)
  // The burst was served before the limit kicked in
  assert.ok(s.messages.length >= 10 && s.messages.length < 30)
})

test('connections per IP are limited', async () => {
  const expectRejected = (headers) => assert.rejects(connect(LIMITED_URL, headers), /429/)

  const a = await connect(LIMITED_URL, { 'X-Forwarded-For': '10.0.0.2' })
  // Rightmost X-Forwarded-For entry is the one added by the trusted proxy
  const b = await connect(LIMITED_URL, { 'X-Forwarded-For': '6.6.6.6, 10.0.0.2' })
  await expectRejected({ 'X-Forwarded-For': '10.0.0.2' })

  // Other addresses are unaffected; an IPv6 /64 counts as one address
  const c = await connect(LIMITED_URL, { 'X-Forwarded-For': '2001:db8::1' })
  const d = await connect(LIMITED_URL, { 'X-Forwarded-For': '2001:db8::2' })
  await expectRejected({ 'X-Forwarded-For': '2001:db8::ffff' })

  // Closing a connection frees a slot
  a.close()
  await wait(100)
  const e = await connect(LIMITED_URL, { 'X-Forwarded-For': '10.0.0.2' })
  for (const s of [b, c, d, e]) s.close()
})
