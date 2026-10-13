const WebSocket = require('ws');
const assert = require('node:assert/strict');

const URL = process.env.TRACKER_URL || 'wss://localhost:8000';
const timeoutMs = 5000;
const announceIntervalSeconds = Number(process.env.TRACKER_ANNOUNCE_INTERVAL || 0);

function createClient() {
  const ws = new WebSocket(URL, { rejectUnauthorized: false });
  const messages = [];
  ws.on('message', raw => {
    try {
      messages.push(JSON.parse(raw.toString()));
    } catch {
      // The test waits for protocol messages and will time out if one is missing.
    }
  });
  return { ws, messages };
}

function waitFor(predicate, description, maxWaitMs = timeoutMs) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started >= maxWaitMs) {
        clearInterval(timer);
        reject(new Error('Timed out waiting for ' + description));
      }
    }, 20);
  });
}

function send(client, message) {
  client.ws.send(JSON.stringify(message));
}

function waitForOpen(client) {
  if (client.ws.readyState === WebSocket.OPEN) return Promise.resolve();
  return new Promise((resolve, reject) => {
    client.ws.once('open', resolve);
    client.ws.once('error', reject);
  });
}

function announceReplies(client, infoHash) {
  return client.messages.filter(
    message => message.action === 'announce' && message.info_hash === infoHash
  );
}

async function scrapeFile(client, infoHash) {
  const countBefore = client.messages.filter(
    message => message.action === 'scrape' && message.files && message.files[infoHash]
  ).length;
  send(client, { action: 'scrape', info_hash: infoHash });
  await waitFor(
    () => client.messages.filter(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    ).length > countBefore,
    'scrape response for ' + infoHash
  );
  return client.messages.filter(
    message => message.action === 'scrape' && message.files && message.files[infoHash]
  ).at(-1).files[infoHash];
}

async function scrapeRequest(client, request) {
  const before = client.messages.filter(message => message.action === 'scrape').length;
  send(client, { action: 'scrape', ...request });
  await waitFor(() => client.messages.filter(message => message.action === 'scrape').length > before,
    'scrape response');
  return client.messages.filter(message => message.action === 'scrape').at(-1).files;
}

async function main() {
  const seed = createClient();
  const partial = createClient();
  const observer = createClient();
  const invalid = createClient();
  const infoHash = 'paused-test-hash-001';
  const seedId = 'paused-test-seed-0001';
  const partialId = 'paused-test-partial-01';

  try {
    await Promise.all([waitForOpen(seed), waitForOpen(partial), waitForOpen(observer)]);

    // Register a complete seed in the swarm first.
    send(seed, {
      action: 'announce',
      event: 'started',
      info_hash: infoHash,
      peer_id: seedId,
      left: 0,
    });
    await waitFor(() => announceReplies(seed, infoHash).length > 0, 'seed announce reply');

    // A partial seed may use paused for its very first announce.
    send(partial, {
      action: 'announce',
      event: 'paused',
      info_hash: infoHash,
      peer_id: partialId,
      left: 1024,
      numwant: 10,
      offers: [{
        offer_id: 'offer-paused-first',
        offer: { type: 'offer', sdp: 'paused-first-sdp' },
      }],
    });

    await waitFor(() => announceReplies(partial, infoHash).length > 0, 'first paused announce reply');
    await waitFor(
      () => seed.messages.some(message => message.offer_id === 'offer-paused-first'),
      'offer from first paused announce'
    );

    let reply = announceReplies(partial, infoHash).at(-1);
    assert.equal(reply.complete, 1, 'the complete seed should remain complete');
    assert.equal(reply.incomplete, 1, 'the paused partial seed should be counted as incomplete');
    assert.equal(seed.messages.find(message => message.offer_id === 'offer-paused-first').peer_id, partialId);
    assert.equal(partial.ws.readyState, WebSocket.OPEN, 'a first paused announce must keep the connection open');

    // Repeated paused announces must refresh the same membership, not duplicate it.
    const beforeRepeat = announceReplies(partial, infoHash).length;
    send(partial, {
      action: 'announce',
      event: 'paused',
      info_hash: infoHash,
      peer_id: partialId,
      left: 1024,
      numwant: 10,
      offers: [],
    });
    await waitFor(
      () => announceReplies(partial, infoHash).length > beforeRepeat,
      'repeated paused announce reply'
    );

    reply = announceReplies(partial, infoHash).at(-1);
    assert.equal(reply.complete, 1);
    assert.equal(reply.incomplete, 1, 'repeated paused announces must not duplicate swarm membership');
    assert.equal((await scrapeFile(observer, infoHash)).downloaders, 0,
      'a paused partial seed must not count as an active downloader');
    assert.equal(partial.ws.readyState, WebSocket.OPEN, 'repeated paused announces must keep the connection open');

    // A regular periodic announce resumes downloading and restores downloader count.
    const beforeResume = announceReplies(partial, infoHash).length;
    send(partial, {
      action: 'announce',
      info_hash: infoHash,
      peer_id: partialId,
      left: 1024,
      offers: [],
    });
    await waitFor(
      () => announceReplies(partial, infoHash).length > beforeResume,
      'regular announce after paused'
    );
    reply = announceReplies(partial, infoHash).at(-1);
    assert.equal((await scrapeFile(observer, infoHash)).downloaders, 1,
      'a resumed partial seed should count as a downloader');

    // Pausing again removes it from the active downloader count.
    const beforePauseAgain = announceReplies(partial, infoHash).length;
    send(partial, {
      action: 'announce',
      event: 'paused',
      info_hash: infoHash,
      peer_id: partialId,
      left: 1024,
      offers: [],
    });
    await waitFor(
      () => announceReplies(partial, infoHash).length > beforePauseAgain,
      'paused announce after resume'
    );
    reply = announceReplies(partial, infoHash).at(-1);
    assert.equal((await scrapeFile(observer, infoHash)).downloaders, 0,
      'pausing again should remove the peer from downloader count');

    // Scrape should report the partial seed as incomplete too.
    send(observer, { action: 'scrape', info_hash: infoHash });
    await waitFor(
      () => observer.messages.some(message => message.action === 'scrape' && message.files && message.files[infoHash]),
      'scrape response'
    );
    const scrape = observer.messages.find(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    );
    assert.equal(scrape.files[infoHash].complete, 1);
    assert.equal(scrape.files[infoHash].incomplete, 1);
    assert.equal(scrape.files[infoHash].downloaders, 0, 'scrape must exclude paused partial seeds from downloaders');

    // An announce carrying an answer is routed as an answer even if event is paused.
    send(partial, {
      action: 'announce',
      event: 'paused',
      info_hash: infoHash,
      peer_id: partialId,
      to_peer_id: seedId,
      offer_id: 'offer-paused-answer',
      answer: { type: 'answer', sdp: 'answer-to-seed' },
    });
    await waitFor(
      () => seed.messages.some(message => message.answer && message.answer.sdp === 'answer-to-seed'),
      'answer carried by a paused announce'
    );
    const answer = seed.messages.find(message => message.answer && message.answer.sdp === 'answer-to-seed');
    assert.equal(answer.peer_id, partialId);
    assert.equal(answer.to_peer_id, undefined);

    // A paused partial seed can later complete; the completed count must change once.
    const beforeCompleted = announceReplies(partial, infoHash).length;
    send(partial, {
      action: 'announce',
      event: 'completed',
      info_hash: infoHash,
      peer_id: partialId,
      left: 0,
      offers: [],
    });
    await waitFor(
      () => announceReplies(partial, infoHash).length > beforeCompleted,
      'completed announce reply after paused'
    );
    reply = announceReplies(partial, infoHash).at(-1);
    assert.equal(reply.complete, 2, 'paused-to-completed must increment the completed count');
    assert.equal(reply.incomplete, 0, 'completed peer must no longer count as incomplete');
    assert.equal((await scrapeFile(observer, infoHash)).downloaders, 0,
      'completed peers must not count as downloaders');

    // Stopping must remove that peer from the swarm.
    send(partial, {
      action: 'announce',
      event: 'stopped',
      info_hash: infoHash,
      peer_id: partialId,
    });
    let scrapeCountBefore = observer.messages.filter(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    ).length;
    send(observer, { action: 'scrape', info_hash: infoHash });
    await waitFor(
      () => observer.messages.filter(
        message => message.action === 'scrape' && message.files && message.files[infoHash]
      ).length > scrapeCountBefore,
      'scrape after stopped announce'
    );
    let latestScrape = observer.messages.filter(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    ).at(-1);
    assert.equal(latestScrape.files[infoHash].complete, 1);
    assert.equal(latestScrape.files[infoHash].incomplete, 0, 'stopped peer must be removed from swarm counts');

    // Rejoin with paused, then disconnect: socket closure must remove membership.
    const beforeRejoin = announceReplies(partial, infoHash).length;
    send(partial, {
      action: 'announce',
      event: 'paused',
      info_hash: infoHash,
      peer_id: partialId,
      left: 1024,
      offers: [],
    });
    await waitFor(
      () => announceReplies(partial, infoHash).length > beforeRejoin,
      'paused reannounce after stopped'
    );
    reply = announceReplies(partial, infoHash).at(-1);
    assert.equal(reply.complete, 1);
    assert.equal(reply.incomplete, 1, 'paused peer must be able to rejoin after stopped');

    const partialClosed = new Promise(resolve => partial.ws.once('close', resolve));
    partial.ws.terminate();
    await partialClosed;

    scrapeCountBefore = observer.messages.filter(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    ).length;
    send(observer, { action: 'scrape', info_hash: infoHash });
    await waitFor(
      () => observer.messages.filter(
        message => message.action === 'scrape' && message.files && message.files[infoHash]
      ).length > scrapeCountBefore,
      'scrape after peer disconnect'
    );
    latestScrape = observer.messages.filter(
      message => message.action === 'scrape' && message.files && message.files[infoHash]
    ).at(-1);
    assert.equal(latestScrape.files[infoHash].complete, 1);
    assert.equal(latestScrape.files[infoHash].incomplete, 0, 'disconnect must remove paused peer from swarm counts');

    // The same peer ID can reconnect and join the swarm again.
    const reconnect = createClient();
    try {
      await waitForOpen(reconnect);
      send(reconnect, {
        action: 'announce',
        event: 'paused',
        info_hash: infoHash,
        peer_id: partialId,
        left: 1024,
        offers: [],
      });
      await waitFor(
        () => announceReplies(reconnect, infoHash).length > 0,
        'paused announce after reconnect'
      );
      reply = announceReplies(reconnect, infoHash).at(-1);
      assert.equal(reply.complete, 1);
      assert.equal(reply.incomplete, 1, 'reconnected peer must be registered exactly once');
    } finally {
      if (reconnect.ws.readyState === WebSocket.OPEN || reconnect.ws.readyState === WebSocket.CONNECTING) {
        reconnect.ws.terminate();
      }
    }

    // BEP 21 scrape accounting: multi-hash and all-swarms paths.
    const multi = createClient();
    const hashA = 'paused-multi-swarm-a';
    const hashB = 'paused-multi-swarm-b';
    const multiId = 'paused-multi-swarm-peer';
    await waitForOpen(multi);
    try {
      send(multi, { action: 'announce', event: 'paused', info_hash: hashA, peer_id: multiId, left: 50, offers: [] });
      send(multi, { action: 'announce', event: 'started', info_hash: hashB, peer_id: multiId, left: 50, offers: [] });
      await waitFor(() => announceReplies(multi, hashA).length && announceReplies(multi, hashB).length,
        'same peer ID registered in two swarms');

      let a = await scrapeFile(observer, hashA);
      let b = await scrapeFile(observer, hashB);
      assert.deepEqual([a.complete, a.incomplete, a.downloaders], [0, 1, 0],
        'paused membership should not count as downloader in swarm A');
      assert.deepEqual([b.complete, b.incomplete, b.downloaders], [0, 1, 1],
        'active membership should count as downloader in swarm B');

      const multiFiles = await scrapeRequest(observer, { info_hash: [hashA, hashB, 'paused-unknown-hash'] });
      assert.deepEqual([multiFiles[hashA].incomplete, multiFiles[hashA].downloaders], [1, 0]);
      assert.deepEqual([multiFiles[hashB].incomplete, multiFiles[hashB].downloaders], [1, 1]);
      assert.deepEqual(multiFiles['paused-unknown-hash'],
        { complete: 0, incomplete: 0, downloaded: 0, downloaders: 0 },
        'unknown hash in multi-hash scrape should return zero counts');

      const allFiles = await scrapeRequest(observer, {});
      assert.equal(allFiles[hashA].downloaders, 0, 'all-swarms scrape should exclude paused peers');
      assert.equal(allFiles[hashB].downloaders, 1, 'all-swarms scrape should count active peers');

      send(multi, { action: 'announce', event: 'stopped', info_hash: hashA, peer_id: multiId });
      a = await scrapeFile(observer, hashA);
      b = await scrapeFile(observer, hashB);
      assert.deepEqual([a.complete, a.incomplete, a.downloaders], [0, 0, 0],
        'stopped membership should be removed from swarm A');
      assert.deepEqual([b.complete, b.incomplete, b.downloaders], [0, 1, 1],
        'stopping swarm A must preserve swarm B membership');
    } finally {
      if (multi.ws.readyState === WebSocket.OPEN || multi.ws.readyState === WebSocket.CONNECTING) {
        const closed = new Promise(resolve => multi.ws.once('close', resolve));
        multi.ws.terminate();
        await closed;
      }
    }

    const afterDisconnect = await scrapeFile(observer, hashB);
    assert.deepEqual([afterDisconnect.complete, afterDisconnect.incomplete, afterDisconnect.downloaders], [0, 0, 0],
      'disconnect must remove remaining multiplexed memberships');

    // The CI interval is short; verify stale paused memberships expire without stale counts.
    if (announceIntervalSeconds > 0) {
      const stale = createClient();
      const staleHash = 'paused-stale-membership';
      try {
        await waitForOpen(stale);
        send(stale, { action: 'announce', event: 'paused', info_hash: staleHash,
          peer_id: 'paused-stale-peer', left: 100, offers: [] });
        await waitFor(() => announceReplies(stale, staleHash).length, 'stale membership announce');
        assert.equal((await scrapeFile(observer, staleHash)).incomplete, 1);
        const deadline = Date.now() + (announceIntervalSeconds * 3 + 4) * 1000;
        let counts;
        do {
          await new Promise(resolve => setTimeout(resolve, 250));
          counts = await scrapeFile(observer, staleHash);
        } while (counts.incomplete !== 0 && Date.now() < deadline);
        assert.equal(counts.incomplete, 0, 'stale paused membership should expire from incomplete');
        assert.equal(counts.downloaders, 0, 'stale paused membership should leave no downloader count');
      } finally {
        if (stale.ws.readyState === WebSocket.OPEN || stale.ws.readyState === WebSocket.CONNECTING) stale.ws.terminate();
      }
    }

    // Paused is now supported; unrelated unknown events must remain rejected.
    await waitForOpen(invalid);
    const invalidClosed = new Promise(resolve => invalid.ws.once('close', resolve));
    send(invalid, {
      action: 'announce',
      event: 'update',
      info_hash: infoHash,
      peer_id: 'paused-test-invalid-01',
      left: 1,
    });
    await Promise.race([
      invalidClosed,
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('Unknown announce event did not close the connection')),
        timeoutMs
      )),
    ]);

    console.log('PASS: paused lifecycle, BEP 21 scrape paths, multi-swarm accounting, stale expiry, cleanup, offer/answer routing, and unknown-event rejection');
  } finally {
    for (const client of [seed, partial, observer, invalid]) {
      if (client.ws.readyState === WebSocket.OPEN || client.ws.readyState === WebSocket.CONNECTING) {
        client.ws.terminate();
      }
    }
  }
}

main().catch(error => {
  console.error('FAIL:', error.message);
  process.exitCode = 1;
});
