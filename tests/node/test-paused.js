
const WebSocket = require('ws');
const assert = require('node:assert/strict');

const ws = new WebSocket('wss://localhost:8000', {
  rejectUnauthorized: false,
});

let passed = false;

const timeout = setTimeout(() => {
  console.error('FAIL: timed out waiting for response');
  ws.terminate();
  process.exitCode = 1;
}, 5000);

ws.on('open', () => {
  ws.send(JSON.stringify({
    action: 'announce',
    event: 'paused',
    info_hash: 'test-info-hash',
  }));
});

ws.on('message', (raw) => {
  try {
    const response = JSON.parse(raw.toString());

    assert.equal(response.action, 'announce');
    assert.equal(response.info_hash, 'test-info-hash');
    assert.equal(
      response['failure reason'],
      'unsupported announce event: paused',
    );
    assert.equal(ws.readyState, WebSocket.OPEN);

    passed = true;
    console.log('PASS: paused rejected; connection remains open');
    clearTimeout(timeout);
    ws.close();
  } catch (error) {
    console.error('FAIL:', error.message);
    clearTimeout(timeout);
    process.exitCode = 1;
    ws.terminate();
  }
});

ws.on('close', () => {
  clearTimeout(timeout);
  if (!passed) {
    console.error('FAIL: tracker closed the connection');
    process.exitCode = 1;
  }
});

ws.on('error', (error) => {
  clearTimeout(timeout);
  console.error('FAIL:', error.message);
  process.exitCode = 1;
});
