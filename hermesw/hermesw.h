#pragma once

#include <memory>

#include <jsi/ScriptStore.h>
#include <jsi/jsi.h>

__declspec(dllexport) std::
    unique_ptr<facebook::jsi::Runtime> makeDynamicPreparedScriptHermesRuntime(
        std::unique_ptr<facebook::jsi::PreparedScriptStore>);

__declspec(dllexport)
    std::unique_ptr<facebook::jsi::Runtime> makeDebugHermesRuntime();