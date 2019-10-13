#pragma once

#include <memory>

#include <jsi/jsi.h>
#include <jsi/ScriptStore.h>

__declspec(dllexport) std::unique_ptr<facebook::jsi::Runtime> makeDynamicPreparedScriptHermesRuntime(
        std::unique_ptr<facebook::jsi::PreparedScriptStore>);