// Flint VS Code extension: launches the Rust language server.
//
// The server is a separate process speaking LSP over stdio. This file is the
// whole client — no TypeScript build step, no bundler — because the extension
// must load fast on a phone-hosted remote, where every millisecond of toolchain
// is real cost.

const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

function serverCommand(context) {
  const config = vscode.workspace.getConfiguration("flint.lsp");
  const configured = config.get("serverPath", "flint-lsp");
  // A path that exists on disk wins over a bare name so a locally built server
  // can be used without touching PATH (the normal case during development).
  const candidate = configured.includes("/") ? configured : `${context.extensionPath}/bin/${configured}`;
  return configured;
}

function start(context) {
  const config = vscode.workspace.getConfiguration("flint.lsp");
  const args = config.get("arguments", ["--flintc", "flintc"]);

  // stdio transport: the server owns stdout, so nothing here may print to it.
  const serverOptions = {
    run: { command: serverCommand(context), args, transport: TransportKind.stdio },
    debug: {
      command: serverCommand(context),
      args,
      transport: TransportKind.stdio,
      options: { cwd: context.extensionPath },
    },
  };

  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "flint" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.fl"),
    },
    outputChannel: vscode.window.createOutputChannel("Flint Language Server"),
  };

  client = new LanguageClient("flintLsp", "flint-lsp", serverOptions, clientOptions);
  context.subscriptions.push(client);
  return client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate: start, deactivate };