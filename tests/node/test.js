// Integration tests: spawns the tracker binary and talks to it over websockets.
// TRACKER_BIN overrides the binary path (default: ../../build/openwebtorrent-tracker).

const { test, before, after } = require('node:test')
const assert = require('node:assert')
const { spawn, execFile } = require('node:child_process')
const path = require('node:path')
const WebSocket = require('ws')

const BIN = process.env.TRACKER_BIN || path.join(__dirname, '../../build/openwebtorrent-tracker')
const PORT = 20000 + Math.floor(Math.random() * 20000)
const URL = `ws://127.0.0.1:${PORT}`
// Second tracker with tight limits
const LIMITED_PORT = PORT + 1
const LIMITED_URL = `ws://127.0.0.1:${LIMITED_PORT}`
// Third tracker for test-peer-ids.js and test-paused.js (short interval to test expiry)
const SUITES_PORT = PORT + 2
const SUITES_INTERVAL = 2

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
  startTracker(LIMITED_PORT, ['--rate-limit', '5', '--rate-burst', '10', '--max-connections-per-ip', '2', '--trust-proxy']),
  startTracker(SUITES_PORT, ['--announce-interval', String(SUITES_INTERVAL), '--allow-full-scrape'])
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

test('protocol errors are counted per reason in the metrics', async () => {
  const s = await connect()
  for (let i = 0; i < 3; i++) s.json({ action: 'bogus' })
  const failed = () => s.messages.filter((m) => m['failure reason'] === 'unknown action').length
  for (let tries = 0; tries < 20 && failed() < 3; tries++) await wait(50)
  assert.strictEqual(failed(), 3)

  // Metrics are sampled every 5 s
  const reasonOf = (metrics, key) => (metrics.failureReasons || []).find((r) => r.reason === key)
  let metrics
  for (let tries = 0; tries < 30; tries++) {
    metrics = await (await fetch(`http://127.0.0.1:${PORT}/stats/metrics.json`)).json()
    if ((reasonOf(metrics, 'unknown_action') || {}).total >= 3) break
    await wait(500)
  }
  const reason = reasonOf(metrics, 'unknown_action')
  assert.ok(reason, 'unknown_action in failureReasons')
  assert.ok(reason.total >= 3)
  assert.strictEqual(reason.message, 'unknown action')
  assert.ok(reason.windowCount >= 3)
  const sum = metrics.failureReasons.reduce((n, r) => n + r.total, 0)
  assert.strictEqual(sum, metrics.totals.failures)
  s.close()
})

test('rejected requests are sampled on /debug/failures.json without SDP', async () => {
  const ua = 'owt-integration-test/1.0'
  const s = await connect(URL, { 'User-Agent': ua, Origin: 'https://example.test' })
  await wait(150) // one sample per reason per 100 ms
  const sdp = 'v=0 c=IN IP4 203.0.113.7 a=candidate:1 1 udp 1 198.51.100.4 5000 typ host'
  s.json({ action: 'announce', event: 'started', info_hash: 'ab'.repeat(20), peer_id: id('h'), numwant: 1, offers: [{ offer_id: 'o', offer: { type: 'offer', sdp } }] })
  await s.next((m) => m['failure reason'] === 'info_hash field is missing or wrong')

  const res = await fetch(`http://127.0.0.1:${PORT}/debug/failures.json`)
  assert.strictEqual(res.status, 200)
  const body = await res.text()
  assert.ok(!body.includes('203.0.113.7') && !body.includes('198.51.100.4'), 'no SDP in samples')
  const sample = JSON.parse(body).announce_info_hash.find((x) => x.client.includes(ua))
  assert.ok(sample, 'sample for this client')
  assert.strictEqual(sample.client, `https://example.test | ${ua}`)
  assert.strictEqual(sample.request.info_hash.bytes, 40)
  assert.strictEqual(sample.request.info_hash.hex, true)
  assert.strictEqual(sample.request.peer_id.preview, id('h'))
  assert.strictEqual(sample.request.offers.size, 1)
  assert.strictEqual(sample.tracker.connectionTorrents, 0)
  assert.ok(sample.connectionAgeSeconds >= 0)
  s.close()
})

// Like bittorrent-tracker, an answer counts as an announce (libtorrent answers
// on a new connection before announcing there)
test('answer on a new connection reaches the offerer', async () => {
  const ih = 'q'.repeat(20)
  const offerer = await connect()
  const oldConn = await connect()
  oldConn.json({ action: 'announce', event: 'started', info_hash: ih, peer_id: id('Q'), numwant: 0, offers: [] })
  await oldConn.next((m) => m.interval)
  offerer.json({ action: 'announce', event: 'started', info_hash: ih, peer_id: id('W'), numwant: 1, offers: [{ offer_id: 'late-offer', offer: { type: 'offer', sdp: 'o' } }] })
  const offer = await oldConn.next((m) => m.offer)
  oldConn.close()
  await wait(100)
  const newConn = await connect()
  newConn.json({ action: 'announce', info_hash: ih, peer_id: id('Q'), to_peer_id: offer.peer_id, offer_id: offer.offer_id, answer: { type: 'answer', sdp: 'late-answer' } })
  const answer = await offerer.next((m) => m.answer)
  assert.strictEqual(answer.answer.sdp, 'late-answer')
  assert.strictEqual(answer.peer_id, id('Q'))
  assert.ok(!newConn.messages.some((m) => m['failure reason']))

  // A peer ID live on another connection can't be used to answer
  const thief = await connect()
  thief.json({ action: 'announce', info_hash: ih, peer_id: id('Q'), to_peer_id: offer.peer_id, offer_id: offer.offer_id, answer: { type: 'answer', sdp: 'stolen' } })
  const failure = await thief.next((m) => m['failure reason'])
  assert.strictEqual(failure['failure reason'], 'answer: peer_id belongs to another connection')
  await wait(100)
  assert.ok(!offerer.messages.some((m) => m.answer && m.answer.sdp === 'stolen'))
  offerer.close(); newConn.close(); thief.close()
})

test('status page and its data are marked noindex', async () => {
  const page = await fetch(`http://127.0.0.1:${PORT}/status`)
  assert.strictEqual(page.headers.get('x-robots-tag'), 'noindex, nofollow')
  assert.match(await page.text(), /<meta name="robots" content="noindex, nofollow">/)
  for (const path of ['/stats/metrics.json', '/stats/history.json?range=1h', '/debug/failures.json']) {
    const res = await fetch(`http://127.0.0.1:${PORT}${path}`)
    assert.strictEqual(res.status, 200, path)
    assert.strictEqual(res.headers.get('x-robots-tag'), 'noindex, nofollow', path)
  }
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

// Multiple peer IDs per connection and BEP 21 suites (contributed in #27)
for (const suite of ['test-peer-ids.js', 'test-paused.js']) {
  test(suite, () => new Promise((resolve, reject) => {
    const env = { ...process.env, TRACKER_URL: `ws://127.0.0.1:${SUITES_PORT}`, TRACKER_ANNOUNCE_INTERVAL: String(SUITES_INTERVAL) }
    execFile(process.execPath, [path.join(__dirname, suite)], { env, timeout: 60000 }, (err, stdout, stderr) => {
      if (err) reject(new Error(`${suite} failed:\n${stdout}${stderr}`))
      else resolve()
    })
  }))
}
