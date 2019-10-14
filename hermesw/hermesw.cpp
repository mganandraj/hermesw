// ConsoleApplication2.cpp : This file contains the 'main' function. Program
// execution begins and ends there.
//

#include <iostream>
#include <thread>

#include <sstream>

#include <jsi/ScriptStore.h>
#include <jsi/decorator.h>

#include <CompileJS.h>
#include <DebuggerAPI.h>
#include <hermes.h>

#include <hermes/inspector/RuntimeAdapter.h>
#include <hermes/inspector/chrome/Connection.h>

#include "transport/ws_session.h"

using namespace facebook;

class StringBuffer : public jsi::Buffer {
 public:
  static std::shared_ptr<const jsi::Buffer> bufferFromString(
      std::string &&str) {
    return std::make_shared<StringBuffer>(std::move(str));
  }

  StringBuffer(std::string str) : str_(std::move(str)){};
  size_t size() const override {
    return str_.size();
  }
  const uint8_t *data() const override {
    return reinterpret_cast<const uint8_t *>(str_.c_str());
  }

 private:
  std::string str_;
};

void dump(const std::string &txt, int indent) {
  for (int i = 0; i < indent; i++)
    std::cout << "  ";
  std::cout << txt << std::endl;
}

void dumpDynamic(const folly::dynamic &dyn) {
  static int indent = 0;
  indent++;

  switch (dyn.type()) {
    case folly::dynamic::NULLT: {
      dump("NULL", indent);
      break;
    }

    case folly::dynamic::ARRAY: {
      dump("ARRAY", indent);
      for (size_t i = 0; i < dyn.size(); ++i) {
        dumpDynamic(dyn[i]);
      }
      break;
    }

    case folly::dynamic::BOOL: {
      std::stringstream ss;
      ss << "BOOL: " << dyn.getBool();
      dump(ss.str(), indent);
      break;
    }

    case folly::dynamic::DOUBLE: {
      std::stringstream ss;
      ss << "DOUBLE: " << dyn.getDouble();
      dump(ss.str(), indent);
      break;
    }

    case folly::dynamic::INT64: {
      // Can't use asDouble() here.  If the int64 value is too bit to be
      // represented precisely as a double, folly will throw an
      // exception.

      std::stringstream ss;
      ss << "INT64: " << dyn.getInt();
      dump(ss.str(), indent);
      break;
    }

    case folly::dynamic::OBJECT: {
      for (const auto &element : dyn.items()) {
        std::stringstream ss;
        ss << "PROPNAME: " << element.first.asString();
        dump(ss.str(), indent);
        dumpDynamic(element.second);
      }

      break;
    }

    case folly::dynamic::STRING: {
      std::stringstream ss;
      ss << "STRING: " << dyn.getString();
      dump(ss.str(), indent);
      break;
    }
  }

  indent--;
}

class DebugHermesRuntime : public facebook::jsi::RuntimeDecorator<
                               facebook::hermes::HermesRuntime,
                               facebook::jsi::Runtime> {
 private:
  class RemoteConnection : public facebook::react::IRemoteConnection {
   public:
    std::shared_ptr<web_socket_session_interface> ws_connection_;
    DebugHermesRuntime &runtime_;

    RemoteConnection(
        std::shared_ptr<web_socket_session_interface> ws_connection,
        DebugHermesRuntime &runtime)
        : ws_connection_(ws_connection), runtime_(runtime) {}

    void onMessage(std::string message) override {
      auto resp = folly::parseJson(message);
      if (resp.at("method") == "Debugger.scriptParsed") {
        folly::dynamic params = resp.at("params");
        assert(params.isObject());

        folly::dynamic scriptId = params.at("scriptId");
        folly::dynamic url = params.at("url");
        assert(scriptId.isString());
        assert(url.isString());

        runtime_.script_id_url_map_.emplace(scriptId.asInt(), url.getString());
      }

      ws_connection_->write(message);
    }

    void onDisconnect() override {}
  };

  void sendMessageToVM(const std::string &line) {
    conn_->sendMessage(line);
  }

  void sendMessageToDebuggerClient(
      const std::string &line,
      web_socket_session_interface &ws_connection) {
    ws_connection.write(line);
  }

  bool handleScriptSourceRequest(
      const std::string &reqStr,
      web_socket_session_interface &ws_connection) {
    auto req = folly::parseJson(reqStr);

    if (req.at("method") == "Debugger.getScriptSource") {
      folly::dynamic result = folly::dynamic::object;

      folly::dynamic params = req.at("params");
      assert(params.isObject());

      folly::dynamic scriptId = params.at("scriptId");
      assert(scriptId.isString());

	  result["scriptSource"] = "<Unable to fetch source>";
      if (script_id_url_map_.find(scriptId.asInt()) !=
          script_id_url_map_.end()) {
        std::string url = script_id_url_map_[scriptId.asInt()];
        if (url_source_map_.find(url) != url_source_map_.end()) {
          result["scriptSource"] = url_source_map_[url];
        } else {
        }
      }

      folly::dynamic resp = folly::dynamic::object;
      resp["id"] = req.at("id");
      resp["result"] = std::move(result);

      sendMessageToDebuggerClient(folly::toJson(resp), ws_connection);

      return true;
    }

    return false;
  }

  void runDebuggerLoop_ws(
      facebook::hermes::inspector::chrome::Connection &conn) {
    create_web_socket_server(
        8888,
        [this](std::shared_ptr<web_socket_session_interface> ws_connection) {
          conn_->connect(
              std::make_unique<RemoteConnection>(ws_connection, *this));

          ws_connection->setOnRead([this, ws_connection](std::string line) {
            if (!handleScriptSourceRequest(line, *ws_connection)) {
              sendMessageToVM(line);
            }
          });
        });
  }

 public:
  DebugHermesRuntime(std::unique_ptr<facebook::hermes::HermesRuntime> base)
      : facebook::jsi::RuntimeDecorator<
            facebook::hermes::HermesRuntime,
            facebook::jsi::Runtime>(*base),
        base_(std::move(base)) {
    base_->getDebugger().setShouldPauseOnScriptLoad(true);

    auto adapter =
        std::make_unique<facebook::hermes::inspector::SharedRuntimeAdapter>(
            base_);

    conn_ = std::make_unique<facebook::hermes::inspector::chrome::Connection>(
        std::move(adapter), "hermes-chrome-debug-server");

    debugger_thread_ = std::thread(
        &DebugHermesRuntime::runDebuggerLoop_ws, this, std::ref(*conn_));
  }

  jsi::Value evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &source,
      const std::string &sourceURL) override {
    facebook::hermes::HermesRuntime::DebugFlags flags;
    std::string source_str(
        reinterpret_cast<const char *>(source->data()), source->size());

    url_source_map_.emplace(sourceURL, source_str);

    base_->debugJavaScript(source_str, sourceURL, flags);

    return jsi::Value::undefined();
  }

 private:
  friend class RemoteConnection;
  std::shared_ptr<facebook::hermes::HermesRuntime> base_;

  // TODO :: Think harder on the lifetime and disconnection	.
  std::unique_ptr<facebook::hermes::inspector::chrome::Connection> conn_;

  std::thread debugger_thread_;

  std::unordered_map<int, std::string> script_id_url_map_;
  std::unordered_map<std::string, std::string> url_source_map_;
};

// Note:: Unfortunately, we can't override from the concrete implementation
// which is HermesRuntimeImpl, which has perf implications due to multiple
// virtual pointer chases!
class DynamicPreparedScriptHermesRuntime
    : public facebook::jsi::RuntimeDecorator<
          facebook::hermes::HermesRuntime,
          facebook::jsi::Runtime> {
 public:
  DynamicPreparedScriptHermesRuntime(
      std::unique_ptr<facebook::hermes::HermesRuntime> base,
      std::unique_ptr<facebook::jsi::PreparedScriptStore> prepared_script_store)
      : facebook::jsi::RuntimeDecorator<
            facebook::hermes::HermesRuntime,
            facebook::jsi::Runtime>(*base),
        base_(std::move(base)),
        prepared_script_store_(std::move(prepared_script_store)) {}

  jsi::Value evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &source,
      const std::string &sourceURL) override {
    jsi::ScriptSignature scriptSignature = {sourceURL, 1};
    jsi::JSRuntimeSignature runtimeSignature = {"Hermes", 21};

    if (!prepared_script_store_ ||
        facebook::hermes::HermesRuntime::isHermesBytecode(
            source->data(), source->size())) {
      return base_->evaluateJavaScript(source, sourceURL);
    }

    std::shared_ptr<const jsi::Buffer> hbc_deser =
        prepared_script_store_->tryGetPreparedScript(
            scriptSignature, runtimeSignature, "perf");

    if (hbc_deser) {
      jsi::Value result = base_->evaluateJavaScript(hbc_deser, sourceURL);
      // TODO :: Check whether the evaluation failed to load the hbc due to
      // version mismatch. If so, regenerate bytecode and retry.
      return result;
    }

    std::string source_str(
        reinterpret_cast<const char *>(source->data()), source->size());
    std::string hbc_compiled;

    bool compile_result =
        ::hermes::compileJS(source_str, sourceURL, hbc_compiled);
    if (!hbc_compiled.empty()) {
      auto hbc_buffer = std::make_shared<StringBuffer>(hbc_compiled);
      prepared_script_store_->persistPreparedScript(
          hbc_buffer, scriptSignature, runtimeSignature, "hermes");

      return base_->evaluateJavaScript(hbc_buffer, sourceURL);
    } else {
      return base_->evaluateJavaScript(source, sourceURL);
    }
  }

 private:
  std::unique_ptr<facebook::hermes::HermesRuntime> base_;
  std::unique_ptr<facebook::jsi::PreparedScriptStore> prepared_script_store_;
};

__declspec(dllexport) std::
    unique_ptr<facebook::jsi::Runtime> makeDynamicPreparedScriptHermesRuntime(
        std::unique_ptr<facebook::jsi::PreparedScriptStore>
            prepared_script_store) {
  return std::make_unique<DynamicPreparedScriptHermesRuntime>(
      facebook::hermes::makeHermesRuntime(), std::move(prepared_script_store));
}

__declspec(dllexport)
    std::unique_ptr<facebook::jsi::Runtime> makeDebugHermesRuntime() {
  return std::make_unique<DebugHermesRuntime>(
      facebook::hermes::makeHermesRuntime());
}