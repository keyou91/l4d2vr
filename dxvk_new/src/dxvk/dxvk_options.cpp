#include "dxvk_options.h"

namespace dxvk {

  DxvkOptions::DxvkOptions(const Config& config) {
    const std::string gplAsyncCacheEnv = env::getEnvVar("DXVK_GPLASYNCCACHE");
    gplAsyncCache = gplAsyncCacheEnv == "0" ? false
      : gplAsyncCacheEnv == "1" ? true
      : config.getOption<bool>("dxvk.gplAsyncCache", true);

    const std::string asyncEnv = env::getEnvVar("DXVK_ASYNC");
    enableAsync = asyncEnv == "0" ? false
      : asyncEnv == "1" ? true
      : config.getOption<bool>("dxvk.enableAsync", true);

    enableDebugUtils      = config.getOption<bool>    ("dxvk.enableDebugUtils",       false);
    enableStateCache      = config.getOption<bool>    ("dxvk.enableStateCache",       true);
    enableMemoryDefrag    = config.getOption<Tristate>("dxvk.enableMemoryDefrag",     Tristate::Auto);
    numCompilerThreads    = config.getOption<int32_t> ("dxvk.numCompilerThreads",     0);
    enableGraphicsPipelineLibrary = config.getOption<Tristate>("dxvk.enableGraphicsPipelineLibrary", Tristate::False);
    trackPipelineLifetime = config.getOption<Tristate>("dxvk.trackPipelineLifetime",  Tristate::Auto);
    useRawSsbo            = config.getOption<Tristate>("dxvk.useRawSsbo",             Tristate::Auto);
    hud                   = config.getOption<std::string>("dxvk.hud", "");
    tearFree              = config.getOption<Tristate>("dxvk.tearFree",               Tristate::Auto);
    latencySleep          = config.getOption<Tristate>("dxvk.latencySleep",           Tristate::False);
    latencyTolerance      = config.getOption<int32_t> ("dxvk.latencyTolerance",       1000);
    disableNvLowLatency2  = config.getOption<Tristate>("dxvk.disableNvLowLatency2",   Tristate::Auto);
    hideIntegratedGraphics = config.getOption<bool>   ("dxvk.hideIntegratedGraphics", false);
    zeroMappedMemory      = config.getOption<bool>    ("dxvk.zeroMappedMemory",       false);
    allowFse              = config.getOption<bool>    ("dxvk.allowFse",               false);
    framePace             = config.getOption<std::string>("dxvk.framePace",           "");
    lowLatencyOffset      = config.getOption<int32_t> ("dxvk.lowLatencyOffset",       0);
    lowLatencyAllowCpuFramesOverlap
                          = config.getOption<bool>    ("dxvk.lowLatencyAllowCpuFramesOverlap", true);
    deviceFilter          = config.getOption<std::string>("dxvk.deviceFilter",        "");
    tilerMode             = config.getOption<Tristate>("dxvk.tilerMode",              Tristate::Auto);
  }

}
