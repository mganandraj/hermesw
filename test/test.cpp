// test.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include <iostream>

#include "hermesw/hermesw.h"
#include "BaseScriptStoreImpl.h"

using namespace facebook;

class StringBuffer : public jsi::Buffer {
 public:
  static std::shared_ptr<const jsi::Buffer> bufferFromString(std::string&& str) {
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


int main() {
  std::string hbc_path = "D:\\hbc\\";

  std::unique_ptr<facebook::jsi::Runtime> runtime =
      makeDynamicPreparedScriptHermesRuntime(
          std::make_unique<facebook::react::BasePreparedScriptStoreImpl>(
              hbc_path));

  std::string js = "print('hello')";
  std::string url = "test.js";

  runtime->evaluateJavaScript(StringBuffer::bufferFromString(std::move(js)), url);
  
}