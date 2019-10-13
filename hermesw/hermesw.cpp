// ConsoleApplication2.cpp : This file contains the 'main' function. Program
// execution begins and ends there.
//

#include <iostream>

#include <jsi/ScriptStore.h>
#include <jsi/decorator.h>

#include <CompileJS.h>
#include <hermes.h>

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

// Note:: Unfortunately, we can't override from the concrete implementation
// which is HermesRuntimeImpl, which has perf implications due to multiple
// virtual pointer chases!
class DynamicPreparedScriptHermesRuntime
    : public facebook::jsi::
          RuntimeDecorator<facebook::jsi::Runtime, facebook::jsi::Runtime> {
 public:
  DynamicPreparedScriptHermesRuntime(
      std::unique_ptr<facebook::hermes::HermesRuntime> base,
      std::unique_ptr<facebook::jsi::PreparedScriptStore> prepared_script_store)
      : facebook::jsi::
            RuntimeDecorator<facebook::jsi::Runtime, facebook::jsi::Runtime>(
                *base),
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