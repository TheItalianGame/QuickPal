const HOST = "com.quickpal.tabs";

let port = null;
let reconnectTimer = null;
let snapshotTimer = null;

function scheduleReconnect() {
  if (reconnectTimer) {
    return;
  }
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, 2000);
}

function post(message) {
  if (!port) {
    scheduleReconnect();
    return;
  }
  try {
    port.postMessage(message);
  } catch {
    port = null;
    scheduleReconnect();
  }
}

async function sendSnapshot() {
  if (!port) {
    scheduleReconnect();
    return;
  }

  const tabs = await chrome.tabs.query({});
  post({
    type: "tabs",
    updatedAt: new Date().toISOString(),
    tabs: tabs
      .filter((tab) => Number.isInteger(tab.id) && Number.isInteger(tab.windowId))
      .map((tab) => ({
        id: tab.id,
        windowId: tab.windowId,
        title: tab.title || "",
        url: tab.url || "",
        active: Boolean(tab.active)
      }))
  });
}

function queueSnapshot(delay = 60) {
  clearTimeout(snapshotTimer);
  snapshotTimer = setTimeout(sendSnapshot, delay);
}

async function activateTab(message) {
  if (!Number.isInteger(message.windowId) || !Number.isInteger(message.tabId)) {
    return;
  }
  try {
    await chrome.windows.update(message.windowId, { focused: true });
    await chrome.tabs.update(message.tabId, { active: true });
  } finally {
    queueSnapshot();
  }
}

async function closeTab(message) {
  if (!Number.isInteger(message.tabId)) {
    return;
  }
  try {
    await chrome.tabs.remove(message.tabId);
  } finally {
    queueSnapshot();
  }
}

async function reloadTab(message) {
  if (!Number.isInteger(message.tabId)) {
    return;
  }
  try {
    await chrome.tabs.reload(message.tabId);
  } finally {
    queueSnapshot();
  }
}

function connect() {
  try {
    port = chrome.runtime.connectNative(HOST);
  } catch {
    port = null;
    scheduleReconnect();
    return;
  }

  port.onMessage.addListener((message) => {
    if (message && message.type === "activate") {
      activateTab(message);
    } else if (message && message.type === "close") {
      closeTab(message);
    } else if (message && message.type === "reload") {
      reloadTab(message);
    }
  });

  port.onDisconnect.addListener(() => {
    port = null;
    scheduleReconnect();
  });

  queueSnapshot(0);
}

chrome.runtime.onInstalled.addListener(() => {
  connect();
});
chrome.runtime.onStartup.addListener(() => {
  connect();
});

chrome.tabs.onCreated.addListener(() => queueSnapshot());
chrome.tabs.onRemoved.addListener(() => queueSnapshot());
chrome.tabs.onMoved.addListener(() => queueSnapshot());
chrome.tabs.onReplaced.addListener(() => queueSnapshot());
chrome.tabs.onActivated.addListener(() => queueSnapshot());
chrome.tabs.onUpdated.addListener((_tabId, changeInfo) => {
  if (changeInfo.title !== undefined || changeInfo.url !== undefined || changeInfo.status === "complete") {
    queueSnapshot();
  }
});
chrome.windows.onFocusChanged.addListener(() => queueSnapshot());
chrome.windows.onRemoved.addListener(() => queueSnapshot());

connect();
