const WebSocket = require('ws');
const assert = require('node:assert/strict');

const URL = process.env.TRACKER_URL || 'wss://localhost:8000';
const wsOptions = { rejectUnauthorized: false };
const timeoutMs = 8000;

function connect() {
  const ws = new WebSocket(URL, wsOptions);
  const messages = [];
  ws.on('message', raw => {
    try { messages.push(JSON.parse(raw.toString())); } catch {}
  });
  return { ws, messages };
}

function waitFor(predicate, description) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started > timeoutMs) {
        clearInterval(timer);
        reject(new Error('Timed out waiting for ' + description));
      }
    }, 20);
  });
}

function opened(client) {
  return new Promise((resolve, reject) => {
    client.ws.once('open', resolve);
    client.ws.once('error', reject);
  });
}

function send(client, message) {
  client.ws.send(JSON.stringify(message));
}

async function scrape(client, infoHash) {
  const before = client.messages.filter(m => m.action === 'scrape' && m.files).length;
  send(client, infoHash === undefined
    ? { action: 'scrape' }
    : { action: 'scrape', info_hash: infoHash });
  await waitFor(
    () => client.messages.filter(m => m.action === 'scrape' && m.files).length > before,
    'scrape response'
  );
  return client.messages.filter(m => m.action === 'scrape' && m.files).slice(-1)[0];
}

async function main() {
  const a = connect();
  const b = connect();
  const c = connect();
  const e = connect();
  const hash1 = 'hash0000000000000001';
  const hash2 = 'hash0000000000000002';
  const hash3 = 'hash0000000000000003';
  const peerA = 'peerA000000000000000';
  const peerB = 'peerB000000000000000';
  const peerC = 'peerC000000000000000';

  try {
    await Promise.all([opened(a), opened(b), opened(c), opened(e)]);

    // One socket may register two distinct peer IDs, each in its own swarm.
    send(a, { action: 'announce', event: 'started', info_hash: hash1, peer_id: peerA, left: 1 });
    send(a, { action: 'announce', event: 'started', info_hash: hash2, peer_id: peerB, left: 1 });
    send(a, { action: 'announce', event: 'started', info_hash: hash3, peer_id: peerB, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && [hash1, hash2, hash3].includes(m.info_hash)).length >= 3,
      'announce replies for multiple peer IDs and swarms on one socket'
    );
    assert.equal(a.ws.readyState, WebSocket.OPEN, 'multiple peer IDs must not close the shared socket');

    // A second socket joins hash1, then receives an offer from peerA.
    send(b, { action: 'announce', event: 'started', info_hash: hash1, peer_id: peerC, left: 1 });
    await waitFor(() => b.messages.some(m => m.action === 'announce' && m.info_hash === hash1), 'peerC announce reply');

    send(a, {
      action: 'announce', info_hash: hash1, peer_id: peerA, numwant: 1,
      offers: [{ offer_id: 'offer-1', offer: { type: 'offer', sdp: 'test-sdp' } }],
    });
    await waitFor(() => b.messages.some(m => m.offer_id === 'offer-1'), 'offer routed to peerC');
    const offer = b.messages.find(m => m.offer_id === 'offer-1');
    assert.equal(offer.peer_id, peerA);
    assert.equal(offer.info_hash, hash1);

    // The answer must route back to peerA and preserve the sender's peer ID.
    send(b, {
      action: 'announce', info_hash: hash1, peer_id: peerC, to_peer_id: peerA,
      answer: { type: 'answer', sdp: 'answer-sdp-1' }, offer_id: 'offer-1',
    });
    await waitFor(() => a.messages.some(m => m.answer && m.answer.sdp === 'answer-sdp-1'), 'answer routed back to peerA');
    const answer = a.messages.find(m => m.answer && m.answer.sdp === 'answer-sdp-1');
    assert.equal(answer.peer_id, peerC);

    // Reconnecting peerA on another socket replaces only peerA, not the old socket.
    send(c, { action: 'announce', event: 'started', info_hash: hash1, peer_id: peerA, left: 1 });
    await waitFor(() => c.messages.some(m => m.action === 'announce' && m.info_hash === hash1), 'peerA takeover reply');
    assert.equal(a.ws.readyState, WebSocket.OPEN, 'taking over peerA must not close its old shared socket');

    // peerB remains registered on the old socket after peerA moves.
    const beforeHash2 = a.messages.filter(m => m.action === 'announce' && m.info_hash === hash2).length;
    send(a, { action: 'announce', event: 'started', info_hash: hash2, peer_id: peerB, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && m.info_hash === hash2).length > beforeHash2,
      'peerB remains active after peerA moves'
    );

    // Answers for the replaced peer ID must now go to its new socket.
    send(b, {
      action: 'announce', info_hash: hash1, peer_id: peerC, to_peer_id: peerA,
      answer: { type: 'answer', sdp: 'answer-sdp-2' }, offer_id: 'offer-2',
    });
    await waitFor(() => c.messages.some(m => m.answer && m.answer.sdp === 'answer-sdp-2'), 'answer routed to peerA new socket');
    assert.equal(a.messages.some(m => m.answer && m.answer.sdp === 'answer-sdp-2'), false);

    // Stopping peerB in hash2 removes only that membership and leaves the socket alive.
    send(a, { action: 'announce', event: 'stopped', info_hash: hash2, peer_id: peerB });
    await new Promise(resolve => setTimeout(resolve, 100));
    assert.equal(a.ws.readyState, WebSocket.OPEN);
    const beforeHash3 = a.messages.filter(m => m.action === 'announce' && m.info_hash === hash3).length;
    send(a, { action: 'announce', event: 'started', info_hash: hash3, peer_id: peerB, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && m.info_hash === hash3).length > beforeHash3,
      'peerB remains active in hash3 after stopping hash2'
    );

    // A different socket must not be able to stop a peer it does not own.
    send(b, { action: 'announce', event: 'stopped', info_hash: hash3, peer_id: peerB });
    await new Promise(resolve => setTimeout(resolve, 100));
    let scrapeResult = await scrape(b, hash3);
    assert.equal(scrapeResult.files[hash3].incomplete, 1,
      'a different connection must not stop peerB');

    // Multiple peer IDs on one socket may independently join the same swarm.
    const hash7 = 'hash0000000000000007';
    const peerG = 'peerG000000000000000';
    const peerH = 'peerH000000000000000';
    send(a, { action: 'announce', event: 'started', info_hash: hash7, peer_id: peerG, left: 1 });
    send(a, { action: 'announce', event: 'started', info_hash: hash7, peer_id: peerH, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && m.info_hash === hash7).length >= 2,
      'two peer IDs from one connection joining the same swarm'
    );
    scrapeResult = await scrape(b, hash7);
    assert.equal(scrapeResult.files[hash7].incomplete, 2,
      'different peer IDs on one socket must be distinct swarm members');

    // Stopping one of those identities must not remove the other identity.
    send(a, { action: 'announce', event: 'stopped', info_hash: hash7, peer_id: peerG });
    await new Promise(resolve => setTimeout(resolve, 100));
    scrapeResult = await scrape(b, hash7);
    assert.equal(scrapeResult.files[hash7].incomplete, 1,
      'stopping one peer ID must preserve the other peer ID in the same swarm');

    // A takeover must remove all old memberships for that peer ID, but leave
    // other peer IDs on the old connection untouched.
    const hash8 = 'hash0000000000000008';
    const hash9 = 'hash0000000000000009';
    const peerI = 'peerI000000000000000';
    send(a, { action: 'announce', event: 'started', info_hash: hash8, peer_id: peerI, left: 1 });
    send(a, { action: 'announce', event: 'started', info_hash: hash9, peer_id: peerI, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && [hash8, hash9].includes(m.info_hash)).length >= 2,
      'peerI registered in two swarms before takeover'
    );
    send(c, { action: 'announce', event: 'started', info_hash: hash8, peer_id: peerI, left: 1 });
    await waitFor(() => c.messages.some(m => m.action === 'announce' && m.info_hash === hash8), 'peerI takeover');
    scrapeResult = await scrape(b, [hash8, hash9]);
    assert.equal(scrapeResult.files[hash8].incomplete, 1,
      'takeover must not leave the old peer membership duplicated');
    assert.equal(scrapeResult.files[hash9].incomplete, 0,
      'takeover must remove the old peer ID from its other swarms');
    assert.equal(a.ws.readyState, WebSocket.OPEN,
      'taking over one peer ID must not close its former shared socket');
    send(a, { action: 'announce', info_hash: hash7, peer_id: peerH, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && m.info_hash === hash7).length >= 3,
      'unrelated peer ID remains usable after another peer ID is taken over'
    );

    // A stopped peer can announce again, and only its own membership is restored.
    send(a, { action: 'announce', event: 'started', info_hash: hash7, peer_id: peerG, left: 1 });
    await waitFor(
      () => a.messages.filter(m => m.action === 'announce' && m.info_hash === hash7).length >= 3,
      'peerG rejoining after stop'
    );
    scrapeResult = await scrape(b, hash7);
    assert.equal(scrapeResult.files[hash7].incomplete, 2,
      'a stopped peer ID must be able to rejoin without duplicating the other peer');

    // Multiple offers should go to distinct eligible peer IDs/connections when
    // enough recipients exist; the sender must not receive its own offers.
    const hash10 = 'hash0000000000000010';
    const peerJ = 'peerJ000000000000000';
    send(c, { action: 'announce', event: 'started', info_hash: hash10, peer_id: peerA, left: 1 });
    send(b, { action: 'announce', event: 'started', info_hash: hash10, peer_id: peerC, left: 1 });
    send(a, { action: 'announce', event: 'started', info_hash: hash10, peer_id: peerB, left: 1 });
    await waitFor(
      () => c.messages.some(m => m.action === 'announce' && m.info_hash === hash10) &&
        b.messages.some(m => m.action === 'announce' && m.info_hash === hash10) &&
        a.messages.some(m => m.action === 'announce' && m.info_hash === hash10),
      'three eligible peers in offer distribution swarm'
    );
    const offersBefore = new Map([
      ['a', a.messages.length], ['b', b.messages.length], ['c', c.messages.length],
    ]);
    send(c, {
      action: 'announce', info_hash: hash10, peer_id: peerA, numwant: 2,
      offers: [
        { offer_id: 'offer-distribution-1', offer: { type: 'offer', sdp: 'sdp-1' } },
        { offer_id: 'offer-distribution-2', offer: { type: 'offer', sdp: 'sdp-2' } },
      ],
    });
    await waitFor(
      () => [a, b].every(client => client.messages.some(m =>
        m.offer_id === 'offer-distribution-1' || m.offer_id === 'offer-distribution-2')),
      'both offers distributed to distinct recipients'
    );
    const receivedA = a.messages.slice(offersBefore.get('a')).filter(m => m.offer_id?.startsWith('offer-distribution-'));
    const receivedB = b.messages.slice(offersBefore.get('b')).filter(m => m.offer_id?.startsWith('offer-distribution-'));
    assert.equal(receivedA.length, 1, 'connection A should receive one offer');
    assert.equal(receivedB.length, 1, 'connection B should receive one offer');
    assert.notEqual(receivedA[0].offer_id, receivedB[0].offer_id,
      'two offers should be assigned to distinct eligible recipients');
    assert.equal(c.messages.slice(offersBefore.get('c')).some(m =>
      m.offer_id?.startsWith('offer-distribution-')), false,
      'the sender must not receive its own offers');

    // Characterize protocol validation: currently, short peer IDs and info hashes
    // are accepted. This is observational coverage, not an endorsement of the behavior.
    const shortHash = 'short-hash';
    const shortPeer = 'short-peer';
    send(e, { action: 'announce', event: 'started', info_hash: shortHash, peer_id: shortPeer, left: 1 });
    await waitFor(
      () => e.messages.some(m => m.action === 'announce' && m.info_hash === shortHash),
      'current short-identifier announce behavior'
    );
    scrapeResult = await scrape(b, shortHash);
    assert.equal(scrapeResult.files[shortHash].incomplete, 1,
      'characterization: short identifiers are currently accepted and registered');
    await new Promise((resolve, reject) => {
      e.ws.once('close', resolve);
      e.ws.once('error', reject);
      e.ws.close();
    });
    scrapeResult = await scrape(b, shortHash);
    assert.equal(scrapeResult.files[shortHash].incomplete, 0,
      'closing the short-identifier connection still cleans up its membership');

    // Preserve completion counts, duplicate-announce behavior, and scrape variants.
    const hash4 = 'hash0000000000000004';
    const unknownHash = 'unknown0000000000000';
    const peerD = 'peerD000000000000000';
    const peerE = 'peerE000000000000000';

    send(c, { action: 'announce', event: 'started', info_hash: hash4, peer_id: peerD, left: 0 });
    send(c, { action: 'announce', event: 'started', info_hash: hash4, peer_id: peerE, left: 1 });
    await waitFor(
      () => c.messages.filter(m => m.action === 'announce' && m.info_hash === hash4).length >= 2,
      'announce replies for completion regression test'
    );

    // Re-announcing an existing peer must not duplicate it or clear completion.
    send(c, { action: 'announce', info_hash: hash4, peer_id: peerD, left: 1 });
    await waitFor(
      () => c.messages.filter(m => m.action === 'announce' && m.info_hash === hash4).length >= 3,
      'repeat announce reply'
    );
    scrapeResult = await scrape(b, hash4);
    assert.equal(scrapeResult.files[hash4].complete, 1, 'left=0 should mark a peer complete');
    assert.equal(scrapeResult.files[hash4].incomplete, 1, 'repeat announce must not duplicate peers');

    // The completed event must mark peerE complete; stopping peerD must not
    // remove peerE from the same swarm.
    send(c, { action: 'announce', event: 'completed', info_hash: hash4, peer_id: peerE, left: 0 });
    await waitFor(
      () => c.messages.filter(m => m.action === 'announce' && m.info_hash === hash4).length >= 4,
      'completed event reply'
    );
    send(c, { action: 'announce', event: 'stopped', info_hash: hash4, peer_id: peerD });
    await new Promise(resolve => setTimeout(resolve, 100));

    scrapeResult = await scrape(b, [hash4, unknownHash]);
    assert.equal(scrapeResult.files[hash4].complete, 1);
    assert.equal(scrapeResult.files[hash4].incomplete, 0);
    assert.equal(scrapeResult.files[unknownHash].complete, 0);
    assert.equal(scrapeResult.files[unknownHash].incomplete, 0);

    scrapeResult = await scrape(b);
    assert.equal(scrapeResult.files[hash4].complete, 1,
      'scraping all swarms should include the active swarm');

    // Closing the shared socket removes all identities still owned by it, not peerA on c.
    await new Promise((resolve, reject) => {
      a.ws.once('close', resolve);
      a.ws.once('error', reject);
      a.ws.close();
    });

    const disconnectScrape = await scrape(b, [hash1, hash3]);
    assert.equal(disconnectScrape.files[hash3].incomplete, 0, 'disconnect should remove peerB from hash3');
    assert.equal(disconnectScrape.files[hash1].incomplete, 2, 'disconnect must not remove peerA from its replacement socket or peerC');

    // Expiry is per membership, not per connection or peer ID. Keep hash7 fresh
    // while allowing hash6 to go stale on the same peer ID and WebSocket.
    const hash6 = 'hash0000000000000006';
    const peerF = 'peerF000000000000000';
    send(c, { action: 'announce', event: 'started', info_hash: hash6, peer_id: peerF, left: 1 });
    send(c, { action: 'announce', event: 'started', info_hash: hash7, peer_id: peerF, left: 1 });
    await waitFor(
      () => c.messages.filter(m => m.action === 'announce' && [hash6, hash7].includes(m.info_hash)).length >= 2,
      'peerF announces both memberships before stale expiry'
    );
    const refresh = setInterval(() => {
      if (c.ws.readyState === WebSocket.OPEN) {
        send(c, { action: 'announce', info_hash: hash7, peer_id: peerF, left: 1 });
      }
    }, 700);
    try {
      await new Promise(resolve => setTimeout(resolve, 6000));
    } finally {
      clearInterval(refresh);
    }
    scrapeResult = await scrape(b, [hash6, hash7]);
    assert.equal(scrapeResult.files[hash6].incomplete, 0,
      'a stale membership must expire without waiting for its WebSocket to close');
    assert.equal(scrapeResult.files[hash7].incomplete, 1,
      'refreshing one membership must preserve it while another membership expires');

    console.log('PASS: multiplexing, offer/answer routing, takeover, isolated stop, disconnect cleanup, identifier validation characterization, per-membership expiry, same-swarm identities, multi-swarm takeover, rejoin after stop, offer distribution, ownership checks, completion counts, duplicate announces, and scrape compatibility');
  } finally {
    a.ws.terminate();
    b.ws.terminate();
    c.ws.terminate();
    e.ws.terminate();
  }
}

main().catch(error => {
  console.error('FAIL:', error.message);
  process.exitCode = 1;
});
