#include "Faaslet.h"

#include <stdexcept>
#include <system/CGroup.h>
#include <system/NetworkNamespace.h>

#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/config.h>
#include <faabric/util/timing.h>
#include <module_cache/WasmModuleCache.h>

#include <wamr/WAMRWasmModule.h>
#include <wavm/WAVMWasmModule.h>

#if (FAASM_SGX)
#include <sgx/SGXWAMRWasmModule.h>
#include <sgx/faasm_sgx_system.h>
#endif

using namespace isolation;

namespace faaslet {
void Faaslet::postFlush()
{
    // Clear shared files
    storage::FileSystem::clearSharedFiles();

    // Clear module cache
    module_cache::getWasmModuleCache().clear();

    // Prepare python runtime if necessary
    scheduler.preflightPythonCall();
}

Faaslet::Faaslet(int threadIdxIn)
  : FaabricExecutor(threadIdxIn)
{
    // Set up network namespace
    isolationIdx = threadIdx + 1;
    std::string netnsName = BASE_NETNS_NAME + std::to_string(isolationIdx);
    ns = std::make_unique<NetworkNamespace>(netnsName);
    ns->addCurrentThread();

    // Add this thread to the cgroup
    CGroup cgroup(BASE_CGROUP_NAME);
    cgroup.addCurrentThread();
}

void Faaslet::postFinish()
{
    ns->removeCurrentThread();
}

void Faaslet::preFinishCall(faabric::Message& call,
                            bool success,
                            const std::string& errorMsg)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    const std::string funcStr = faabric::util::funcToString(call, true);

    // Add captured stdout if necessary
    faabric::util::SystemConfig& conf = faabric::util::getSystemConfig();
    if (conf.captureStdout == "on") {
        std::string moduleStdout = module->getCapturedStdout();
        if (!moduleStdout.empty()) {
            std::string newOutput = moduleStdout + "\n" + call.outputdata();
            call.set_outputdata(newOutput);

            module->clearCapturedStdout();
        }
    }

    if (conf.wasmVm == "wavm") {
        // Restore from zygote
        logger->debug("Resetting module {} from zygote", funcStr);
        module_cache::WasmModuleCache& registry =
          module_cache::getWasmModuleCache();
        wasm::WAVMWasmModule& cachedModule = registry.getCachedModule(call);

        auto* wavmModulePtr = dynamic_cast<wasm::WAVMWasmModule*>(module.get());
        *wavmModulePtr = cachedModule;
    }
}

void Faaslet::postBind(const faabric::Message& msg, bool force)
{
    faabric::util::SystemConfig& conf = faabric::util::getSystemConfig();

    // Instantiate the right wasm module for the chosen runtime
    if (conf.wasmVm == "wamr") {
#if (FAASM_SGX)
        // When SGX is enabled, we may still be running with vanilla WAMR
        if (msg.issgx()) {
            module = std::make_unique<wasm::SGXWAMRWasmModule>();
        } else {
            module = std::make_unique<wasm::WAMRWasmModule>();
        }
#else
        // Vanilla WAMR
        module = std::make_unique<wasm::WAMRWasmModule>();
#endif

        module->bindToFunction(msg);
    } else if (conf.wasmVm == "wavm") {
        PROF_START(snapshotRestore)

        // Load snapshot from cache
        module_cache::WasmModuleCache& registry =
          module_cache::getWasmModuleCache();
        wasm::WAVMWasmModule& snapshot = registry.getCachedModule(msg);

        // Use snapshot to restore WAVM module
        module = std::make_unique<wasm::WAVMWasmModule>(snapshot);

        PROF_END(snapshotRestore)
    } else {
        faabric::util::getLogger()->error("Unrecognised wasm VM: {}",
                                          conf.wasmVm);
        throw std::runtime_error("Unrecognised wasm VM");
    }
}

bool Faaslet::doExecute(faabric::Message& msg)
{
    auto logger = faabric::util::getLogger();

    // Check if we need to restore from a different snapshot
    auto conf = faabric::util::getSystemConfig();
    if (conf.wasmVm == "wavm") {
        const std::string snapshotKey = msg.snapshotkey();
        if (!snapshotKey.empty() && !msg.issgx()) {
            PROF_START(snapshotOverride)

            module_cache::WasmModuleCache& registry =
              module_cache::getWasmModuleCache();
            wasm::WAVMWasmModule& snapshot = registry.getCachedModule(msg);
            module = std::make_unique<wasm::WAVMWasmModule>(snapshot);

            PROF_END(snapshotOverride)
        }
    }

    // Execute the function
    bool success = module->execute(msg);
    return success;
}
}
