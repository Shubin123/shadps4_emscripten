// WebGPU side of the browser build's renderer (video_core/renderer_web/web_gpu.cpp).
//
// The GPU device lives on the page's main thread: WebGPU's promises (device creation, error
// scopes, buffer readback) only settle on a thread whose event loop runs, and the emulator's own
// threads are pthreads (workers) busy in C++. They post jobs here with MAIN_THREAD_ASYNC_EM_ASM and
// block on a futex word in linear memory until the job's results are back in guest memory.
//
// Before the runtime starts, a device is requested unless the page set Module.shadps4Gpu itself
// (to share its device with presentation) or set Module.shadps4GpuMode = "off". Without one, the
// rasterizer drops GPU work as it did before this backend existed.

if (typeof WorkerGlobalScope === "undefined") {
  Module["preRun"] = [].concat(Module["preRun"] || [], function () {
    if (Module["shadps4Gpu"] || Module["shadps4GpuMode"] === "off" ||
        typeof navigator === "undefined" || !navigator.gpu) {
      return;
    }
    addRunDependency("shadps4-webgpu");
    shadps4GpuCreateDevice().then(function (gpu) {
      Module["shadps4Gpu"] = gpu;
    }, function (e) {
      console.warn("[webgpu] no device: " + e);
    }).finally(function () {
      removeRunDependency("shadps4-webgpu");
    });
  });
}

async function shadps4GpuCreateDevice() {
  var adapter = await navigator.gpu.requestAdapter({powerPreference: "high-performance"});
  if (!adapter) {
    throw new Error("no adapter");
  }
  // GCN workgroups go up to 1024 threads and 64KB of LDS, and guest buffers can be large: ask
  // for everything the adapter has rather than WebGPU's conservative defaults.
  var wanted = ["maxComputeInvocationsPerWorkgroup", "maxComputeWorkgroupSizeX",
                "maxComputeWorkgroupSizeY", "maxComputeWorkgroupSizeZ",
                "maxComputeWorkgroupStorageSize", "maxComputeWorkgroupsPerDimension",
                "maxStorageBufferBindingSize", "maxBufferSize", "maxStorageBuffersPerShaderStage",
                "maxStorageTexturesPerShaderStage", "maxSampledTexturesPerShaderStage",
                "maxSamplersPerShaderStage", "maxBindingsPerBindGroup"];
  var limits = {};
  for (var name of wanted) {
    if (adapter.limits[name] !== undefined) {
      limits[name] = adapter.limits[name];
    }
  }
  var features = [];
  if (adapter.features.has("subgroups")) {
    features.push("subgroups");
  }
  var device = await adapter.requestDevice({requiredFeatures: features, requiredLimits: limits});
  device.lost.then(function (info) {
    console.error("[webgpu] device lost: " + info.message);
  });
  var ai = adapter.info || {};
  return {device: device, pipelines: new Map(),
          description: [ai.vendor, ai.architecture, ai.description].filter(Boolean).join(" ")};
}

function shadps4GpuU64(i) {
  return HEAPU32[i] + HEAPU32[i + 1] * 4294967296;
}

// Runs one compute job (layout: struct JsComputeJob in web_gpu.cpp) and signals its done word:
// 1 = finished and written back, 2 = failed (logged here).
Module["shadps4GpuCompute"] = async function (job) {
  var j = job / 4;
  var wgsl_ptr = shadps4GpuU64(j), wgsl_len = shadps4GpuU64(j + 2);
  var key = HEAPU32[j + 4] + ":" + HEAPU32[j + 5];
  var gx = HEAPU32[j + 6], gy = HEAPU32[j + 7], gz = HEAPU32[j + 8];
  var count = HEAPU32[j + 9];
  var list = shadps4GpuU64(j + 10) / 4;
  var done = shadps4GpuU64(j + 12) / 4;
  var gpu = Module["shadps4Gpu"];
  var status = 1;
  var buffers = [];
  try {
    var device = gpu.device;
    var bindings = [];
    for (var i = 0; i < count; i++) {
      var b = list + i * 6; // {u64 ptr, u64 size, u32 binding, u32 writable}
      bindings.push({ptr: shadps4GpuU64(b), size: shadps4GpuU64(b + 2), binding: HEAPU32[b + 4],
                     writable: HEAPU32[b + 5] !== 0});
    }
    var entry = gpu.pipelines.get(key);
    if (!entry) {
      var code = new TextDecoder().decode(HEAPU8.slice(wgsl_ptr, wgsl_ptr + wgsl_len));
      if (Module["shadps4GpuDumpShaders"]) {
        console.log("[webgpu] shader " + key + ":\n" + code);
      }
      device.pushErrorScope("validation");
      var layout = device.createBindGroupLayout({entries: bindings.map(function (b) {
        return {binding: b.binding, visibility: GPUShaderStage.COMPUTE,
                buffer: {type: b.writable ? "storage" : "read-only-storage"}};
      })});
      var pipeline = device.createComputePipeline({
        layout: device.createPipelineLayout({bindGroupLayouts: [layout]}),
        compute: {module: device.createShaderModule({code: code}), entryPoint: "main"},
      });
      var error = await device.popErrorScope();
      entry = error ? {error: error.message} : {pipeline: pipeline, layout: layout};
      gpu.pipelines.set(key, entry);
      if (error) {
        console.error("[webgpu] compute pipeline " + key + ": " + error.message);
      }
    }
    if (entry.error) {
      throw new Error("pipeline " + key + " failed to build");
    }

    device.pushErrorScope("validation");
    var entries = [];
    for (var b of bindings) {
      var size = Math.max(16, Math.ceil(b.size / 4) * 4);
      var buffer = device.createBuffer({size: size, usage: GPUBufferUsage.STORAGE |
                                        GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC});
      device.queue.writeBuffer(buffer, 0, HEAPU8.slice(b.ptr, b.ptr + Math.ceil(b.size / 4) * 4));
      buffers.push(buffer);
      entries.push({binding: b.binding, resource: {buffer: buffer}});
    }
    var encoder = device.createCommandEncoder();
    var pass = encoder.beginComputePass();
    pass.setPipeline(entry.pipeline);
    pass.setBindGroup(0, device.createBindGroup({layout: entry.layout, entries: entries}));
    pass.dispatchWorkgroups(gx, gy, gz);
    pass.end();
    var readbacks = [];
    bindings.forEach(function (b, i) {
      if (!b.writable || b.size === 0) {
        return;
      }
      var size = Math.ceil(b.size / 4) * 4;
      var staging = device.createBuffer({size: size,
                                         usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST});
      encoder.copyBufferToBuffer(buffers[i], 0, staging, 0, size);
      buffers.push(staging);
      readbacks.push({b: b, staging: staging});
    });
    device.queue.submit([encoder.finish()]);
    var error = await device.popErrorScope();
    if (error) {
      throw new Error("dispatch: " + error.message);
    }
    await Promise.all(readbacks.map(function (r) { return r.staging.mapAsync(GPUMapMode.READ); }));
    for (var r of readbacks) {
      HEAPU8.set(new Uint8Array(r.staging.getMappedRange(), 0, r.b.size), r.b.ptr);
    }
  } catch (e) {
    console.error("[webgpu] " + e);
    status = 2;
  }
  for (var buffer of buffers) {
    buffer.destroy();
  }
  Atomics.store(HEAP32, done, status);
  Atomics.notify(HEAP32, done);
};
