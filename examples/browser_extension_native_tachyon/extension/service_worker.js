import initTachyonWasm, { WasmBus } from "./vendor/@tachyon-ipc/core/wasm/tachyon_ipc.js";

const HOST_NAME = "tachyon.native_messaging_host";
const CAPACITY = 1 << 20;
const MSG_SIZE = 16;
const PAGE_TO_SW_TYPE = (20 << 16) | 1;
const SW_TO_NATIVE_TYPE = (20 << 16) | 2;
const NATIVE_TO_SW_TYPE = (20 << 16) | 3;
const SW_TO_PAGE_TYPE = (20 << 16) | 4;

let nativePort;
let inFlightNativeId = 0;
let inFlightPageId = 0;
let inFlightPagePort;
let tachyonReady;
let pageInbound;
let nativeOutbound;
let nativeInbound;
let pageOutbound;
let memoryWords;
let readId = 0;
let readValue = 0;
const nativeOutboundMessage = { id: 0, value: 0 };
const pageOutboundMessage = { id: 0, value: 0 };

async function ensureTachyon() {
  if (tachyonReady) return tachyonReady;
  tachyonReady = initTachyonWasm().then((wasm) => {
    pageInbound = new WasmBus(CAPACITY);
    nativeOutbound = new WasmBus(CAPACITY);
    nativeInbound = new WasmBus(CAPACITY);
    pageOutbound = new WasmBus(CAPACITY);
    memoryWords = new Uint32Array(wasm.memory.buffer);
  });
  return tachyonReady;
}

function writeMessage(bus, id, value, typeId) {
  const offset = bus.acquireTx(MSG_SIZE) >>> 2;
  memoryWords[offset] = id >>> 0;
  memoryWords[offset + 1] = value >>> 0;
  memoryWords[offset + 2] = 0;
  memoryWords[offset + 3] = typeId >>> 0;
  bus.commitTx(MSG_SIZE, typeId);
}

function readMessage(bus) {
  if (!bus.acquireRx()) throw new Error("service worker Tachyon ring was empty");
  const offset = bus.rxPtr() >>> 2;
  readId = memoryWords[offset];
  readValue = memoryWords[offset + 1];
  bus.commitRx();
}

function clearInFlight() {
  inFlightNativeId = 0;
  inFlightPageId = 0;
  inFlightPagePort = undefined;
}

function connectNativeHost() {
  if (nativePort) return nativePort;

  nativePort = chrome.runtime.connectNative(HOST_NAME);
  nativePort.onMessage.addListener((message) => {
    if (inFlightNativeId !== message.id) return;
    const pagePort = inFlightPagePort;
    const pageId = inFlightPageId;
    clearInFlight();
    writeMessage(nativeInbound, message.id, message.value, NATIVE_TO_SW_TYPE);
    readMessage(nativeInbound);
    writeMessage(pageOutbound, pageId, readValue, SW_TO_PAGE_TYPE);
    readMessage(pageOutbound);
    pageOutboundMessage.id = readId;
    pageOutboundMessage.value = readValue;
    pagePort.postMessage(pageOutboundMessage);
  });
  nativePort.onDisconnect.addListener(() => {
    const error =
      chrome.runtime.lastError?.message ?? "native messaging host disconnected";
    nativePort = undefined;
    if (inFlightNativeId) {
      inFlightPagePort.postMessage({
        op: "error",
        id: inFlightPageId,
        message: error,
      });
      clearInFlight();
    }
  });

  return nativePort;
}

function attachPagePort(pagePort) {
  if (pagePort.name !== "tachyon-page") return;

  pagePort.onMessage.addListener((message) => {
    if (!memoryWords) {
      ensureTachyon()
        .then(() => handlePageMessage(pagePort, message))
        .catch((error) => reportPageError(pagePort, message.id, error));
      return;
    }
    handlePageMessage(pagePort, message);
  });

  pagePort.onDisconnect.addListener(() => {
    if (inFlightPagePort === pagePort) {
      clearInFlight();
    }
  });
}

function reportPageError(pagePort, id, error) {
  pagePort.postMessage({
    op: "error",
    id,
    message: error.message,
  });
}

function handlePageMessage(pagePort, message) {
  if (inFlightNativeId) {
    throw new Error("native request already in flight");
  }
  writeMessage(pageInbound, message.id, message.value, PAGE_TO_SW_TYPE);
  readMessage(pageInbound);
  const inboundId = readId;
  const inboundValue = readValue;
  writeMessage(nativeOutbound, inboundId, inboundValue, SW_TO_NATIVE_TYPE);
  readMessage(nativeOutbound);
  const outboundId = readId;
  const outboundValue = readValue;
  inFlightNativeId = outboundId;
  inFlightPagePort = pagePort;
  inFlightPageId = message.id;
  nativeOutboundMessage.id = outboundId;
  nativeOutboundMessage.value = outboundValue;
  connectNativeHost().postMessage(nativeOutboundMessage);
}

chrome.runtime.onConnectExternal.addListener(attachPagePort);
void ensureTachyon();
