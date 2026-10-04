// A minimal MCP Apps HOST for the widget tests: the same official AppBridge, the same order
// of operations (srcdoc, connect, wait for ui/notifications/initialized, then the tool
// result) as Moebius's gui/src/bridge.js, so a widget that renders here renders there.
// Bundled by the spec with esbuild and injected into a blank page.
import { AppBridge, PostMessageTransport } from "@modelcontextprotocol/ext-apps/app-bridge";

window.widgetContexts = [];

window.mountWidget = async (html, toolResult, theme) => {
  const iframe = document.getElementById("w");
  const bridge = new AppBridge(null, { name: "faraday-widget-test", version: "0" },
    { openLinks: {}, serverTools: {}, logging: {} }, { hostContext: { theme } });
  bridge.onupdatemodelcontext = async (params) => { window.widgetContexts.push(params); return {}; };
  bridge.oncalltool = async () => { throw new Error("this widget must not call tools"); };
  await new Promise((resolve) => {
    iframe.addEventListener("load", resolve, { once: true });
    iframe.srcdoc = html;
  });
  const initialized = new Promise((resolve) => bridge.addEventListener("initialized", resolve));
  await bridge.connect(new PostMessageTransport(iframe.contentWindow, iframe.contentWindow));
  await initialized;
  await bridge.sendToolResult(toolResult);
};
