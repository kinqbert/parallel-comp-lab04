import { connect } from "net";

const CMD_INIT = 0x01;
const CMD_RUN = 0x02;
const CMD_CHECK = 0x03;
const CMD_RESULT = 0x04;

const RSP_OK = 0x10;
const RSP_ERR = 0x11;
const RSP_BUSY = 0x12;
const RSP_DONE = 0x13;

const N = 4;
const THREAD_COUNT = 4;

function generateMatrix(n) {
  return Array.from({ length: n * n }, () =>
    Math.floor(Math.random() * 100 + 1)
  );
}

function sendAll(socket, buffer) {
  return new Promise((resolve, reject) => {
    socket.write(buffer, (err) => (err ? reject(err) : resolve()));
  });
}

function makeRecvAll(socket) {
  let stash = Buffer.alloc(0);

  return function recvAll(len) {
    return new Promise((resolve) => {
      if (stash.length >= len) {
        const out = stash.slice(0, len);
        stash = stash.slice(len);
        return resolve(out);
      }

      function onData(chunk) {
        stash = Buffer.concat([stash, chunk]);
        if (stash.length >= len) {
          socket.off("data", onData);
          const out = stash.slice(0, len);
          stash = stash.slice(len);
          resolve(out);
        }
      }
      socket.on("data", onData);
    });
  };
}

async function runClient() {
  const matrix = generateMatrix(N);
  const client = connect(8080, "127.0.0.1");

  client.on("connect", async () => {
    try {
      const header = Buffer.alloc(9);

      header.writeUInt8(CMD_INIT, 0);
      header.writeUInt32BE(THREAD_COUNT, 1);
      header.writeUInt32BE(N, 5);

      const recvAll = makeRecvAll(client);

      const payload = Buffer.alloc(matrix.length * 4);
      matrix.forEach((val, i) => payload.writeInt32BE(val, i * 4));

      await sendAll(client, header);
      await sendAll(client, payload);

      const [resp] = await recvAll(1);
      if (resp !== RSP_OK) throw new Error("INIT failed");
      console.log("[NODE CLIENT] INIT OK");

      await sendAll(client, Buffer.from([CMD_RUN]));
      const [runResp] = await recvAll(1);
      if (runResp !== RSP_OK) throw new Error("RUN failed");
      console.log("[NODE CLIENT] RUN accepted");

      while (true) {
        await new Promise((r) => setTimeout(r, 1000));
        await sendAll(client, Buffer.from([CMD_CHECK]));
        const [status] = await recvAll(1);

        if (status === RSP_BUSY) {
          console.log("[NODE CLIENT] still working…");
        }

        if (status === RSP_DONE) break;
      }

      await sendAll(client, Buffer.from([CMD_RESULT]));
      const [resStatus] = await recvAll(1);
      if (resStatus !== RSP_DONE) throw new Error("Result failed");

      const dimBuf = await recvAll(4);
      const n = dimBuf.readUInt32BE();
      const mtxBuf = await recvAll(n * n * 4);

      const received = Array.from({ length: n }, (_, i) =>
        Array.from({ length: n }, (_, j) => mtxBuf.readInt32BE((i * n + j) * 4))
      );

      console.log(`[NODE CLIENT] Sent matrix:`);
      for (let i = 0; i < n; i++) {
        console.log(matrix.slice(i * n, (i + 1) * n).join("\t"));
      }
      console.log(`[NODE CLIENT] Received matrix:`);
      received.forEach((row) => console.log(row.join("\t")));

      client.end();
    } catch (err) {
      console.error(`[NODE CLIENT] Error: ${err.message}`);
      client.end();
    }
  });
}

runClient();
