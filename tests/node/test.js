const WebSocket = require('ws');

const NUM_CLIENTS = 10;

let connectedClients = 0;

for (let i = 0; i < NUM_CLIENTS; i++) {
	const socket = new WebSocket('wss://localhost:8000', {
		rejectUnauthorized: false,
		//strictSSL: false,
	});
	socket.onopen = () => {
		connectedClients++;

		if (connectedClients === NUM_CLIENTS) console.log('Works!');
	};

	socket.onmessage = (e) => {
		//const json = JSON.parse(e.data);
	};

	socket.onclose = () => {
		// Nothing
	}
}
